#include "lower.h"
#include <ctype.h>
#include <limits.h>

/* ---------------------------------------------------------------------
 * Symbols and context
 * ------------------------------------------------------------------- */

enum { SK_VAR, SK_PARAM, SK_GLOBAL, SK_CONST, SK_FUNC, SK_CLASS, SK_MODULE, SK_MACRO, SK_FIELD };
static const char *sk_name[] = { "variable", "parameter", "global", "constant", "function", "class", "module", "macro", "field" };

typedef VEC(const char *) NameVec;

typedef struct {
    const char *name, *ir;
    unsigned char ty, el, rank, kind, global;
    int line, col, info;          /* info: index into LowerInfo.syms */
    long long cval;
    int has_cval;
    Opnd cval_op;                 /* constant value (enum / #define / Java constants) */
} Sym;

typedef struct {
    const char *name;             /* lookup key: "f", "Main.f", "Point.dist" */
    const char *ir;               /* IR function name */
    Ty ret;
    int nparams, variadic, is_static, cls_method;
    VEC(unsigned char) ptys;
    NameVec pnames;
    VEC(Node *) defaults;         /* Python default values (NULL = required) */
} FnSig;

typedef struct {
    Lang lang;
    IRProg *P;
    Func *fn;
    Diags *ds;
    LowerInfo *info;
    VEC(Sym) syms;                /* visible symbols, innermost last */
    IntVec marks;                 /* scope boundaries in syms */
    VEC(Sym) globals;
    VEC(FnSig) fns;
    const char *cls;              /* current class */
    IntVec brk, cont;
    int line;
    Ty ret;
    const char *scope;
    NameVec locals;               /* Python: names local to the current function */
    NameVec gdecl;                /* Python: names declared 'global' in the current function */
    int py_module;                /* lowering module-level Python code */
    NameVec classes;
} Lw;

static Opnd gen(Lw *L, Node *n);
static void stmt(Lw *L, Node *n);
static void branch_false(Lw *L, Node *c, int lab);
static void branch_true(Lw *L, Node *c, int lab);

static int has_name(const char **v, int n, const char *s) { for (int i = 0; i < n; i++) if (v[i] == s) return 1; return 0; }

/* ---- emission ---- */

static Instr *emit(Lw *L, IOp op) {
    Instr in;
    memset(&in, 0, sizeof(in));
    in.op = (unsigned char)op;
    in.line = L->line;
    in.id = L->P->next_id++;
    vpush(L->fn->code, in);
    return &L->fn->code.v[L->fn->code.n - 1];
}

static Opnd temp(Lw *L, Ty ty) { Opnd o = o_none(); o.k = OP_TEMP; o.id = ++L->fn->ntemps; o.ty = (unsigned char)ty; return o; }
static int label(Lw *L) { return ++L->P->nlabels; }
static void put_label(Lw *L, int l) { emit(L, I_LABEL)->lab = l; }
static void put_goto(Lw *L, int l) { emit(L, I_GOTO)->lab = l; }
static void put_if(Lw *L, Opnd a, int l, int when_true) { Instr *i = emit(L, when_true ? I_IF : I_IFNOT); i->a = a; i->lab = l; }
static void put_copy(Lw *L, Opnd d, Opnd a) { Instr *i = emit(L, I_COPY); i->d = d; i->a = a; i->ty = d.ty; }
static void put_param(Lw *L, Opnd a) { emit(L, I_PARAM)->a = a; }

static Opnd put_bin(Lw *L, BinOp op, Opnd a, Opnd b, Ty rty, Ty sem) {
    Opnd d = temp(L, rty);
    Instr *i = emit(L, I_BIN);
    i->sub = (unsigned char)op; i->d = d; i->a = a; i->b = b; i->ty = (unsigned char)sem;
    return d;
}
static Opnd put_un(Lw *L, UnOp op, Opnd a, Ty rty) {
    Opnd d = temp(L, rty);
    Instr *i = emit(L, I_UN);
    i->sub = (unsigned char)op; i->d = d; i->a = a; i->ty = (unsigned char)rty;
    return d;
}
static Opnd put_call(Lw *L, const char *fn, int nargs, Ty rty, int want) {
    Instr *i = emit(L, I_CALL);
    i->fn = intern(fn); i->nargs = nargs; i->ty = (unsigned char)rty;
    if (!want) return o_none();
    Opnd d = temp(L, rty);
    L->fn->code.v[L->fn->code.n - 1].d = d;
    return d;
}

static Opnd opaque(Lw *L, Node *n, const char *what, int want) {
    char desc[160];
    snprintf(desc, sizeof(desc), "%s", what);
    Instr *i = emit(L, I_OPAQUE);
    i->fn = intern(desc);
    i->line = n ? n->line : L->line;
    Opnd d = o_none();
    if (want) { d = temp(L, L->lang == LANG_PY ? TY_DYN : TY_UNKNOWN); L->fn->code.v[L->fn->code.n - 1].d = d; }
    L->info->opaque++;
    L->P->opaque++;
    diag(L->ds, SEV_WARNING, "tac", "Unsupported Construct", n ? n->line : L->line, n ? n->col : 0,
         "Lexing and parsing still cover this code; only TAC translation stops here.",
         "%s is not translated to three-address code; kept as an opaque operation with unknown side effects", what);
    return d;
}

/* ---- symbols ---- */

static void scope_push(Lw *L) { vpush(L->marks, L->syms.n); }
static void scope_pop(Lw *L) { L->syms.n = L->marks.v[--L->marks.n]; }

static int record_sym(Lw *L, const char *name, int kind, const char *type, int line, int col) {
    SymInfo si = { name, sk_name[kind], type ? type : "", L->scope, line, col, 0 };
    vpush(L->info->syms, si);
    return L->info->syms.n - 1;
}

static Sym *lookup(Lw *L, const char *name) {
    for (int i = L->syms.n - 1; i >= 0; i--) if (L->syms.v[i].name == name) return &L->syms.v[i];
    for (int i = L->globals.n - 1; i >= 0; i--) if (L->globals.v[i].name == name) return &L->globals.v[i];
    return NULL;
}

static FnSig *find_fn(Lw *L, const char *name, int nargs) {
    FnSig *any = NULL;
    for (int i = 0; i < L->fns.n; i++) {
        FnSig *f = &L->fns.v[i];
        if (f->name != name) continue;
        if (nargs < 0 || f->nparams == nargs || (f->variadic && nargs >= f->nparams)) return f;
        any = f;
    }
    return any;
}

static Opnd sym_opnd(Sym *s) {
    if (s->has_cval) return s->cval_op;
    Opnd o = o_var(s->ir, (Ty)s->ty, s->global);
    o.el = s->el; o.rank = s->rank;
    return o;
}

static int edit_distance(const char *a, const char *b) {
    int n = (int)strlen(a), m = (int)strlen(b);
    if (n > 40 || m > 40) return 99;
    int d[41][41];
    for (int i = 0; i <= n; i++) d[i][0] = i;
    for (int j = 0; j <= m; j++) d[0][j] = j;
    for (int i = 1; i <= n; i++)
        for (int j = 1; j <= m; j++) {
            int c = d[i - 1][j - 1] + (a[i - 1] != b[j - 1]);
            if (d[i - 1][j] + 1 < c) c = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < c) c = d[i][j - 1] + 1;
            d[i][j] = c;
        }
    return d[n][m];
}

/* "Did you mean 'average'?" when one visible name is a likely misspelling */
static const char *suggest(Lw *L, const char *name, char *buf, size_t n, const char *fallback) {
    const char *best = NULL;
    int bd = 3;
    for (int pass = 0; pass < 2; pass++) {
        int cnt = pass ? L->globals.n : L->syms.n;
        for (int i = 0; i < cnt; i++) {
            const char *cand = pass ? L->globals.v[i].name : L->syms.v[i].name;
            int d = edit_distance(name, cand);
            int limit = strlen(name) <= 3 ? 1 : 2;
            if (d > 0 && d <= limit && d < bd) { bd = d; best = cand; }
        }
    }
    if (!best) return fallback;
    snprintf(buf, n, "Did you mean '%s'?", best);
    return buf;
}

static void use_sym(Lw *L, Sym *s) { if (s->info >= 0 && s->info < L->info->syms.n) L->info->syms.v[s->info].uses++; }

/* ---- types ---- */

static Ty base_ty(Lang lang, const char *t) {
    if (lang == LANG_PY) return TY_DYN;
    if (!t) return TY_UNKNOWN;
    if (strstr(t, "double")) return TY_DOUBLE;
    if (strstr(t, "float")) return TY_FLOAT;
    if (strstr(t, "bool") || strstr(t, "_Bool")) return TY_BOOL;
    if (strstr(t, "char")) return TY_CHAR;
    if (strstr(t, "String")) return TY_STR;
    if (strstr(t, "long") || strstr(t, "size_t") || strstr(t, "int64") || strstr(t, "ptrdiff")) return TY_LONG;
    if (strstr(t, "unsigned") || strstr(t, "uint")) return TY_UINT;
    if (strstr(t, "int") || strstr(t, "short") || strstr(t, "byte") || strstr(t, "signed")) return TY_INT;
    if (strstr(t, "void")) return TY_VOID;
    if (strcmp(t, "var") == 0) return TY_UNKNOWN;
    return TY_OBJ;
}

/* "int[][]" -> ARR(el INT, rank 2); C "char*" -> STR */
static void type_of(Lw *L, const char *t, Ty *ty, Ty *el, int *rank) {
    int r = 0;
    if (t) for (const char *c = t; *c; c++) { if (*c == '*') r++; if (*c == '[') r++; }
    Ty b = base_ty(L->lang, t);
    if (L->lang == LANG_C && b == TY_CHAR && r > 0) { b = TY_STR; r--; }
    *el = b;
    *rank = r;
    *ty = r ? TY_ARR : b;
}

static Ty arith_ty(Lw *L, Ty a, Ty b) {
    if (L->lang == LANG_PY) return TY_DYN;
    if (a == TY_DOUBLE || b == TY_DOUBLE) return TY_DOUBLE;
    if (a == TY_FLOAT || b == TY_FLOAT) return TY_FLOAT;
    if (a == TY_LONG || b == TY_LONG) return TY_LONG;
    if (L->lang == LANG_C && (a == TY_UINT || b == TY_UINT)) return TY_UINT;
    return TY_INT;
}

static Ty cmp_ty(Lw *L) { return L->lang == LANG_C ? TY_INT : L->lang == LANG_JAVA ? TY_BOOL : TY_DYN; }

/* Converts v to type `to` the way an assignment would (C/Java). */
static Opnd convert(Lw *L, Opnd v, Ty to) {
    if (L->lang == LANG_PY || to == v.ty || to == TY_UNKNOWN || to == TY_OBJ || to == TY_VOID || v.ty == TY_UNKNOWN) return v;
    if (to == TY_ARR || to == TY_STR || v.ty == TY_ARR || v.ty == TY_STR || v.ty == TY_NULL || v.ty == TY_OBJ) return v;
    if (to == TY_DOUBLE || to == TY_FLOAT) {
        if (v.k == OP_INT) return o_flt(to == TY_FLOAT ? (float)v.i : (double)v.i, to);
        if (v.k == OP_FLT) return o_flt(to == TY_FLOAT ? (float)v.f : v.f, to);
        if (v.ty == TY_DOUBLE || v.ty == TY_FLOAT) return to == TY_FLOAT ? put_un(L, U_TOFLOAT, v, to) : (v.ty == TY_FLOAT ? put_un(L, U_TODBL, v, to) : v);
        return put_un(L, to == TY_FLOAT ? U_TOFLOAT : U_TODBL, v, to);
    }
    if (to == TY_CHAR) {
        if (v.k == OP_INT) { long long c = L->lang == LANG_JAVA ? (v.i & 0xFFFF) : (signed char)v.i; return o_int(c, TY_CHAR); }
        return put_un(L, U_TOCHAR, v, TY_CHAR);
    }
    if (to == TY_BOOL) {
        if (L->lang == LANG_JAVA) return v;
        if (v.k == OP_INT) return o_int(v.i != 0, TY_BOOL);
        return put_un(L, U_TOBOOL, v, TY_BOOL);
    }
    /* integer targets */
    if (v.k == OP_INT) {
        long long x = v.i;
        if (to == TY_INT) x = (int)(unsigned)x;
        else if (to == TY_UINT) x = (unsigned)x;
        return o_int(x, to);
    }
    if (v.k == OP_FLT) return o_int(to == TY_LONG ? (long long)v.f : (int)v.f, to);
    if (v.ty == TY_DOUBLE || v.ty == TY_FLOAT) return put_un(L, to == TY_LONG ? U_TOLONG : U_TOINT, v, to);
    if (to == TY_LONG) { v.ty = TY_LONG; return v; }
    if (v.ty == TY_LONG || (v.ty == TY_CHAR && L->lang == LANG_JAVA) || (to == TY_UINT) != (v.ty == TY_UINT))
        return put_un(L, U_TOINT, v, to);
    v.ty = (unsigned char)to;
    return v;
}

/* a type as the source language spells it, for diagnostics */
static const char *src_ty(Lw *L, Ty t) {
    if (L->lang == LANG_JAVA) {
        if (t == TY_STR) return "String";
        if (t == TY_BOOL) return "boolean";
        if (t == TY_ARR) return "array";
    }
    if (L->lang == LANG_C) {
        if (t == TY_STR) return "char *";
        if (t == TY_BOOL) return "_Bool";
        if (t == TY_UINT) return "unsigned int";
        if (t == TY_ARR) return "array";
    }
    return ty_name(t);
}

static int java_narrowing(Ty from, Ty to) {
    int rf = from == TY_DOUBLE ? 4 : from == TY_FLOAT ? 3 : from == TY_LONG ? 2 : from == TY_INT ? 1 : 0;
    int rt = to == TY_DOUBLE ? 4 : to == TY_FLOAT ? 3 : to == TY_LONG ? 2 : to == TY_INT ? 1 : to == TY_CHAR ? 0 : -1;
    return rt >= 0 && rf > rt && ty_is_num(from);
}

/* Assignment compatibility (Java is strict, C converts, Python is dynamic). */
static Opnd assign_conv(Lw *L, Node *at, Opnd v, Ty to, const char *what) {
    if (L->lang == LANG_JAVA && to != TY_UNKNOWN && v.ty != TY_UNKNOWN) {
        if (java_narrowing((Ty)v.ty, to) && !(v.k == OP_INT && (to == TY_CHAR || to == TY_INT) && v.ty == TY_INT))
            diag(L->ds, SEV_ERROR, "semantic", "Type Error", at->line, at->col, "Add an explicit cast, e.g. (int) value.",
                 "incompatible types: possible lossy conversion from %s to %s in %s", src_ty(L, (Ty)v.ty), src_ty(L, to), what);
        else if ((to == TY_STR && ty_is_num((Ty)v.ty)) || (ty_is_num(to) && v.ty == TY_STR) ||
                 (to == TY_BOOL && ty_is_num((Ty)v.ty) && v.ty != TY_BOOL) || (ty_is_num(to) && to != TY_BOOL && v.ty == TY_BOOL))
            diag(L->ds, SEV_ERROR, "semantic", "Type Error", at->line, at->col, NULL,
                 "incompatible types: %s cannot be converted to %s in %s", src_ty(L, (Ty)v.ty), src_ty(L, to), what);
    }
    if (L->lang == LANG_C && ((to == TY_STR && ty_is_num((Ty)v.ty) && v.k != OP_INT) || (ty_is_num(to) && v.ty == TY_STR)))
        diag(L->ds, SEV_WARNING, "semantic", "Type Warning", at->line, at->col, NULL,
             "%s mixes a pointer/string with an integer", what);
    return convert(L, v, to);
}

/* ---------------------------------------------------------------------
 * Expressions
 * ------------------------------------------------------------------- */

static const char *py_builtins[] = {
    "print", "len", "range", "abs", "min", "max", "int", "float", "str", "bool", "sum", "sorted", "list",
    "tuple", "enumerate", "zip", "map", "filter", "round", "divmod", "ord", "chr", "isinstance", "type",
    "input", "open", "repr", "hex", "bin", "oct", "pow", "any", "all", "reversed", "set", "dict", "hash",
    "id", "iter", "next", "super", "object", "Exception", "ValueError", "TypeError", "KeyError",
    "IndexError", "RuntimeError", "ZeroDivisionError", "StopIteration", "NotImplementedError",
    "AssertionError", "ArithmeticError", "OverflowError", "frozenset", "format", "vars", "globals",
    "callable", "getattr", "setattr", "hasattr", "staticmethod", "classmethod", "property", "exit", "quit", NULL };
static const char *c_known[] = {
    "printf", "puts", "putchar", "strlen", "abs", "labs", "sqrt", "pow", "fabs", "floor", "ceil", "fmax",
    "fmin", "exit", "malloc", "calloc", "realloc", "free", "memset", "memcpy", "strcpy", "strncpy", "strcmp",
    "strncmp", "strcat", "scanf", "rand", "srand", "time", "sprintf", "snprintf", "fprintf", "fopen",
    "fclose", "fgets", "getchar", "atoi", "atof", "qsort", "assert", "isdigit", "isalpha", "isspace",
    "toupper", "tolower", "sin", "cos", "tan", "exp", "log", "log10", "round", "fmod", "clock", NULL };

static int in_list(const char *s, const char **list) { for (int i = 0; list[i]; i++) if (strcmp(s, list[i]) == 0) return 1; return 0; }

static Opnd lit_int(Lw *L, Node *n) {
    if (L->lang == LANG_PY) return o_int(n->ival, TY_INT);
    Ty t = TY_INT;
    if (n->type && strcmp(n->type, "long") == 0) t = TY_LONG;
    else if (n->s && strpbrk(n->s, "uU")) t = TY_UINT;
    if (t == TY_INT && (n->ival > INT_MAX || n->ival < INT_MIN)) {
        t = TY_LONG;
        if (L->lang == LANG_JAVA && !(n->s && (n->s[0] == '0') && n->ival <= 0xFFFFFFFFLL))
            diag(L->ds, SEV_ERROR, "semantic", "Literal Range", n->line, n->col, "Add an L suffix for a long literal.",
                 "integer number too large: %s", n->s);
        else if (L->lang == LANG_JAVA) { t = TY_INT; n->ival = (int)(unsigned)n->ival; }
    }
    return o_int(n->ival, t);
}

/* known constants that are written as names / members */
static int named_constant(Lw *L, const char *name, Opnd *out) {
    static const struct { Lang l; const char *n; int kind; long long i; double f; } k[] = {
        {LANG_C, "true", 0, 1, 0}, {LANG_C, "false", 0, 0, 0}, {LANG_C, "NULL", 2, 0, 0},
        {LANG_C, "INT_MAX", 0, INT_MAX, 0}, {LANG_C, "INT_MIN", 0, INT_MIN, 0}, {LANG_C, "EOF", 0, -1, 0},
        {LANG_C, "M_PI", 1, 0, 3.14159265358979323846},
        {LANG_JAVA, "Integer.MAX_VALUE", 0, INT_MAX, 0}, {LANG_JAVA, "Integer.MIN_VALUE", 0, INT_MIN, 0},
        {LANG_JAVA, "Long.MAX_VALUE", 3, LLONG_MAX, 0}, {LANG_JAVA, "Long.MIN_VALUE", 3, LLONG_MIN, 0},
        {LANG_JAVA, "Math.PI", 1, 0, 3.14159265358979323846}, {LANG_JAVA, "Math.E", 1, 0, 2.718281828459045},
        {LANG_PY, "math.pi", 1, 0, 3.141592653589793}, {LANG_PY, "math.e", 1, 0, 2.718281828459045},
        {LANG_PY, "math.inf", 1, 0, 1.0 / 0.0}, {LANG_PY, "__name__", 4, 0, 0},
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (k[i].l != L->lang || strcmp(k[i].n, name)) continue;
        if (k[i].kind == 0) *out = o_int(k[i].i, TY_INT);
        else if (k[i].kind == 1) *out = o_flt(k[i].f, TY_DOUBLE);
        else if (k[i].kind == 2) *out = o_null();
        else if (k[i].kind == 3) *out = o_int(k[i].i, TY_LONG);
        else *out = o_str(intern("__main__"));
        return 1;
    }
    return 0;
}

/* dotted text of a Name/Member chain ("System.out.println"), or NULL */
static const char *dotted(Node *n, Buf *b) {
    if (n->kind == N_Name) { bputs(b, n->s); return b->s; }
    if (n->kind == N_Member && !n->ival && n->k.n == 1) {
        if (!dotted(n->k.v[0], b)) return NULL;
        bputc(b, '.'); bputs(b, n->s);
        return b->s;
    }
    return NULL;
}

static int is_module(Lw *L, const char *name) {
    Sym *s = lookup(L, name);
    return s && s->kind == SK_MODULE;
}

/* Python: is this name local in the current function? */
static int py_local(Lw *L, const char *name) {
    return !L->py_module && has_name(L->locals.v, L->locals.n, name) && !has_name(L->gdecl.v, L->gdecl.n, name);
}

static Opnd name_ref(Lw *L, Node *n) {
    Opnd k;
    if (L->lang == LANG_PY) {
        if (py_local(L, n->s)) {
            Sym *s = lookup(L, n->s);
            if (s) { use_sym(L, s); return sym_opnd(s); }
            return o_var(n->s, TY_DYN, 0);
        }
        Sym *s = lookup(L, n->s);
        if (s) { use_sym(L, s); if (s->kind == SK_FUNC || s->kind == SK_CLASS) return opaque(L, n, "a function or class used as a value", 1); return sym_opnd(s); }
        if (named_constant(L, n->s, &k)) return k;
        if (in_list(n->s, py_builtins)) return opaque(L, n, "a builtin function used as a value", 1);
        char hint[128];
        diag(L->ds, SEV_WARNING, "semantic", "Undefined Name", n->line, n->col, suggest(L, n->s, hint, sizeof(hint), "Assign the name before using it."),
             "name '%s' is not defined anywhere in this file", n->s);
        return o_var(n->s, TY_DYN, 1);
    }
    Sym *s = lookup(L, n->s);
    if (s) {
        use_sym(L, s);
        if (s->kind == SK_MACRO) return opaque(L, n, "use of a function-like macro", 1);
        if (s->kind == SK_FUNC) return opaque(L, n, "a function used as a value (function pointer)", 1);
        if (s->kind == SK_FIELD) return opaque(L, n, "instance field access", 1);
        return sym_opnd(s);
    }
    if (named_constant(L, n->s, &k)) return k;
    if (L->lang == LANG_JAVA && L->cls) {                     /* static field of the current class */
        char q[256]; snprintf(q, sizeof(q), "%s.%s", L->cls, n->s);
        Sym *f = lookup(L, intern(q));
        if (f) { use_sym(L, f); return sym_opnd(f); }
    }
    char hint[128];
    diag(L->ds, SEV_ERROR, "semantic", "Undeclared Variable", n->line, n->col,
         suggest(L, n->s, hint, sizeof(hint), "Declare it first, e.g.  int x = 0;"),
         "'%s' is used but never declared", n->s);
    return o_var(n->s, TY_UNKNOWN, 0);
}

/* ---- lvalues ---- */

typedef struct { int kind; Opnd var, base, idx; Node *node; } LVal;   /* kind: 0 var, 1 element, 2 opaque */

static LVal lval(Lw *L, Node *n, int for_store) {
    LVal lv;
    memset(&lv, 0, sizeof(lv));
    lv.node = n;
    if (n->kind == N_Name) {
        if (L->lang == LANG_PY) {
            Sym *s = lookup(L, n->s);
            if (py_local(L, n->s)) {
                if (!s || s->global) {
                    Sym ns = {0};
                    ns.name = n->s; ns.ir = n->s; ns.ty = TY_DYN; ns.kind = SK_VAR; ns.line = n->line; ns.col = n->col;
                    ns.info = record_sym(L, n->s, SK_VAR, "dynamic", n->line, n->col);
                    vpush(L->syms, ns);
                    s = &L->syms.v[L->syms.n - 1];
                }
            } else if (!s || s->kind != SK_GLOBAL) {
                Sym ns = {0};
                ns.name = n->s; ns.ir = n->s; ns.ty = TY_DYN; ns.kind = SK_GLOBAL; ns.global = 1; ns.line = n->line; ns.col = n->col;
                const char *saved = L->scope; L->scope = "module";
                ns.info = record_sym(L, n->s, SK_GLOBAL, "dynamic", n->line, n->col);
                L->scope = saved;
                vpush(L->globals, ns);
                s = &L->globals.v[L->globals.n - 1];
            }
            if (!for_store) use_sym(L, s);
            lv.var = sym_opnd(s);
            return lv;
        }
        Sym *s = lookup(L, n->s);
        if (!s && L->lang == LANG_JAVA && L->cls) {
            char q[256]; snprintf(q, sizeof(q), "%s.%s", L->cls, n->s);
            s = lookup(L, intern(q));
        }
        if (!s) {
            char hint[128];
            diag(L->ds, SEV_ERROR, "semantic", "Undeclared Variable", n->line, n->col, suggest(L, n->s, hint, sizeof(hint), "Declare it before assigning to it."),
                 "'%s' is assigned but never declared", n->s);
            lv.var = o_var(n->s, TY_UNKNOWN, 0);
            return lv;
        }
        if (s->kind == SK_FIELD) { lv.kind = 2; return lv; }
        if (s->kind != SK_VAR && s->kind != SK_PARAM && s->kind != SK_GLOBAL) {
            diag(L->ds, SEV_ERROR, "semantic", "Invalid Assignment", n->line, n->col, NULL, "cannot assign to %s '%s'", sk_name[s->kind], n->s);
            lv.kind = 2;
            return lv;
        }
        if (!for_store) use_sym(L, s);
        lv.var = sym_opnd(s);
        return lv;
    }
    if (n->kind == N_Index && n->k.v[1]->kind != N_Slice) {
        lv.kind = 1;
        lv.base = gen(L, n->k.v[0]);
        lv.idx = gen(L, n->k.v[1]);
        return lv;
    }
    if (n->kind == N_Unary && n->s && strcmp(n->s, "*") == 0) {     /* *p, modelled as p[0] */
        lv.kind = 1;
        lv.base = gen(L, n->k.v[0]);
        lv.idx = o_int(0, TY_INT);
        return lv;
    }
    lv.kind = 2;
    return lv;
}

static Ty elem_ty(Lw *L, Opnd base, Opnd *as) {
    Opnd r = o_none();
    if (L->lang == LANG_PY || base.ty == TY_DYN) { r.ty = TY_DYN; *as = r; return TY_DYN; }
    if (base.ty == TY_STR) { r.ty = TY_CHAR; *as = r; return TY_CHAR; }
    if (base.ty == TY_ARR && base.rank > 1) { r.ty = TY_ARR; r.el = base.el; r.rank = (unsigned char)(base.rank - 1); *as = r; return TY_ARR; }
    if (base.ty == TY_ARR) { r.ty = base.el; *as = r; return (Ty)base.el; }
    r.ty = TY_UNKNOWN; *as = r;
    return TY_UNKNOWN;
}

static Opnd lval_load(Lw *L, LVal *lv) {
    if (lv->kind == 0) return lv->var;
    if (lv->kind == 2) return opaque(L, lv->node, "reading a field or member", 1);
    Opnd shape;
    Ty et = elem_ty(L, lv->base, &shape);
    Opnd d = temp(L, et);
    d.el = shape.el; d.rank = shape.rank;
    Instr *i = emit(L, I_LOAD);
    i->d = d; i->a = lv->base; i->b = lv->idx; i->ty = (unsigned char)et;
    return d;
}

static void lval_store(Lw *L, LVal *lv, Opnd v) {
    if (lv->kind == 0) { put_copy(L, lv->var, v); return; }
    if (lv->kind == 2) { opaque(L, lv->node, "assignment to a field or member", 0); return; }
    Instr *i = emit(L, I_STORE);
    i->a = lv->base; i->b = lv->idx; i->c = v;
    i->ty = (unsigned char)(lv->base.ty == TY_ARR ? lv->base.el : TY_DYN);
}

static Ty lval_ty(Lw *L, LVal *lv) {
    if (lv->kind == 0) return (Ty)lv->var.ty;
    if (lv->kind == 1) { Opnd s; return elem_ty(L, lv->base, &s); }
    return TY_UNKNOWN;
}

/* ---- operators ---- */

static int binop_of(const char *s, BinOp *out) {
    static const struct { const char *s; BinOp b; } t[] = {
        {"+", B_ADD}, {"-", B_SUB}, {"*", B_MUL}, {"/", B_DIV}, {"%", B_MOD}, {"//", B_FLOORDIV}, {"**", B_POW},
        {"&", B_AND}, {"|", B_OR}, {"^", B_XOR}, {"<<", B_SHL}, {">>", B_SHR}, {">>>", B_USHR},
        {"==", B_EQ}, {"!=", B_NE}, {"<", B_LT}, {"<=", B_LE}, {">", B_GT}, {">=", B_GE},
        {"in", B_IN}, {"not in", B_NOTIN}, {"is", B_IS}, {"is not", B_ISNOT} };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) if (strcmp(t[i].s, s) == 0) { *out = t[i].b; return 1; }
    return 0;
}

/* a <op> b with the language's typing rules */
static Opnd binary_op(Lw *L, Node *at, BinOp op, Opnd a, Opnd b) {
    if (L->lang == LANG_PY) return put_bin(L, op, a, b, op >= B_EQ ? TY_DYN : TY_DYN, TY_DYN);
    if (op == B_ADD && L->lang == LANG_JAVA && (a.ty == TY_STR || b.ty == TY_STR))
        return put_bin(L, B_ADD, a, b, TY_STR, TY_STR);                /* string concatenation */
    if ((a.ty == TY_ARR || a.ty == TY_STR || b.ty == TY_ARR || b.ty == TY_STR) && op <= B_USHR) {
        if (L->lang == LANG_C) return opaque(L, at, "pointer arithmetic", 1);
        diag(L->ds, SEV_ERROR, "semantic", "Type Error", at->line, at->col, NULL,
             "bad operand types for binary operator '%s': %s and %s", binop_sym(op), src_ty(L, (Ty)a.ty), src_ty(L, (Ty)b.ty));
    }
    if (op >= B_EQ) {
        Ty t = arith_ty(L, (Ty)a.ty, (Ty)b.ty);
        if (a.ty == TY_BOOL && b.ty == TY_BOOL) t = TY_BOOL;
        if (ty_is_num((Ty)a.ty) && ty_is_num((Ty)b.ty)) { a = convert(L, a, t); b = convert(L, b, t); }
        return put_bin(L, op, a, b, cmp_ty(L), t);
    }
    if (op == B_AND || op == B_OR || op == B_XOR || op == B_SHL || op == B_SHR || op == B_USHR || op == B_MOD) {
        if (op != B_MOD || L->lang == LANG_C) {
            if ((a.ty == TY_DOUBLE || a.ty == TY_FLOAT || b.ty == TY_DOUBLE || b.ty == TY_FLOAT))
                diag(L->ds, SEV_ERROR, "semantic", "Type Error", at->line, at->col, NULL,
                     "operator '%s' needs integer operands, not floating point", binop_sym(op));
        }
    }
    if (L->lang == LANG_JAVA && a.ty == TY_BOOL && b.ty == TY_BOOL && (op == B_AND || op == B_OR || op == B_XOR))
        return put_bin(L, op, a, b, TY_BOOL, TY_BOOL);
    Ty t = (op == B_SHL || op == B_SHR || op == B_USHR) ? arith_ty(L, (Ty)a.ty, TY_INT) : arith_ty(L, (Ty)a.ty, (Ty)b.ty);
    if (op != B_SHL && op != B_SHR && op != B_USHR) b = convert(L, b, t);
    a = convert(L, a, t);
    return put_bin(L, op, a, b, t, t);
}

/* ---- calls ---- */

static void push_args(Lw *L, Opnd *args, int n) { for (int i = 0; i < n; i++) put_param(L, args[i]); }

static Opnd user_call(Lw *L, Node *n, FnSig *f, int first_arg, Opnd *recv, int want) {
    int nargs = n->k.n - first_arg;
    Opnd args[64];
    int na = 0;
    if (recv) args[na++] = *recv;
    if (L->lang == LANG_PY) {
        /* bind positional, keyword and default arguments to parameters */
        int np = f->nparams;
        Node *bound[64] = {0};
        int ok = np <= 60;
        int pos = recv ? 1 : 0;
        for (int i = first_arg; ok && i < n->k.n; i++) {
            Node *a = n->k.v[i];
            if (a->kind == N_Star) { ok = 0; break; }
            if (a->kind == N_KwArg) {
                int j;
                for (j = 0; j < np; j++) if (f->pnames.v[j] == a->s) break;
                if (j == np) { diag(L->ds, SEV_ERROR, "semantic", "Call Error", a->line, a->col, NULL, "%s() got an unexpected keyword argument '%s'", f->name, a->s); ok = 0; break; }
                bound[j] = a->k.v[0];
            } else if (pos < np) bound[pos++] = a;
            else { diag(L->ds, SEV_ERROR, "semantic", "Call Error", a->line, a->col, NULL, "%s() takes %d positional arguments but more were given", f->name, np); ok = 0; break; }
        }
        if (!ok) return opaque(L, n, "a call whose arguments cannot be bound statically", want);
        for (int j = recv ? 1 : 0; j < np; j++) {
            Node *a = bound[j] ? bound[j] : f->defaults.v[j];
            if (!a) { diag(L->ds, SEV_ERROR, "semantic", "Call Error", n->line, n->col, NULL, "%s() missing required argument '%s'", f->name, f->pnames.v[j]); return opaque(L, n, "an incomplete call", want); }
            if (!bound[j] && a->kind != N_IntLit && a->kind != N_FloatLit && a->kind != N_StrLit && a->kind != N_BoolLit && a->kind != N_NullLit)
                return opaque(L, n, "a call relying on a non-constant default argument", want);
            args[na++] = gen(L, a);
        }
    } else {
        if (!f->variadic && nargs != f->nparams - (recv ? 1 : 0))
            diag(L->ds, SEV_ERROR, "semantic", "Call Error", n->line, n->col, "Pass exactly the declared parameters.",
                 "%s() expects %d argument%s but %d %s given", f->name, f->nparams, f->nparams == 1 ? "" : "s", nargs, nargs == 1 ? "was" : "were");
        for (int i = first_arg; i < n->k.n && na < 64; i++) {
            Opnd v = gen(L, n->k.v[i]);
            int pi = na;
            if (pi < f->ptys.n) v = assign_conv(L, n->k.v[i], v, (Ty)f->ptys.v[pi], "an argument");
            args[na++] = v;
        }
    }
    push_args(L, args, na);
    Opnd r = put_call(L, f->ir, na, f->ret == TY_VOID ? TY_VOID : f->ret, want && f->ret != TY_VOID);
    if (want && f->ret == TY_VOID) {
        if (L->lang != LANG_PY) diag(L->ds, SEV_ERROR, "semantic", "Type Error", n->line, n->col, NULL, "%s() returns void; its result cannot be used", f->name);
        return o_int(0, TY_INT);
    }
    return r;
}

/* the result type of a library call we know */
static Ty lib_ret(Lw *L, const char *fn, Opnd *args, int n) {
    if (L->lang == LANG_PY) return TY_DYN;
    static const char *dbl[] = { "sqrt", "pow", "fabs", "floor", "ceil", "fmax", "fmin", "sin", "cos", "tan", "exp", "log",
                                 "log10", "fmod", "atof", "round", "Math.sqrt", "Math.pow", "Math.floor", "Math.ceil",
                                 "Math.sin", "Math.cos", "Math.exp", "Math.log", "Math.random", "Math.hypot", NULL };
    static const char *in[] = { "strlen", "abs", "printf", "putchar", "puts", "atoi", "strcmp", "rand", "isdigit", "isalpha",
                                "isspace", "toupper", "tolower", "getchar", "Integer.parseInt", "String.length", "String.indexOf",
                                "String.compareTo", "Math.round", NULL };
    if (in_list(fn, dbl)) return TY_DOUBLE;
    if (in_list(fn, in)) return strcmp(fn, "Math.round") == 0 ? TY_LONG : TY_INT;
    if (!strcmp(fn, "Math.max") || !strcmp(fn, "Math.min") || !strcmp(fn, "Math.abs")) {
        Ty t = TY_INT;
        for (int i = 0; i < n; i++) t = arith_ty(L, t, (Ty)args[i].ty);
        return t;
    }
    if (!strcmp(fn, "String.charAt")) return TY_CHAR;
    if (!strcmp(fn, "String.equals") || !strcmp(fn, "String.isEmpty") || !strcmp(fn, "String.contains") ||
        !strcmp(fn, "String.startsWith") || !strcmp(fn, "Character.isDigit") || !strcmp(fn, "Character.isLetter")) return L->lang == LANG_JAVA ? TY_BOOL : TY_INT;
    if (!strncmp(fn, "String.", 7) || !strcmp(fn, "Arrays.toString") || !strcmp(fn, "Integer.toString") || !strcmp(fn, "String.valueOf")) return TY_STR;
    if (!strcmp(fn, "System.currentTimeMillis") || !strcmp(fn, "System.nanoTime")) return TY_LONG;
    return TY_UNKNOWN;
}

static Opnd call_expr(Lw *L, Node *n, int want) {
    Node *callee = n->k.v[0];
    Opnd args[64];
    int na = 0;

    if (callee->kind == N_Name) {
        const char *name = callee->s;
        if (L->lang == LANG_JAVA && L->cls) {
            char q[256]; snprintf(q, sizeof(q), "%s.%s", L->cls, name);
            FnSig *f = find_fn(L, intern(q), n->k.n - 1);
            if (!f) f = find_fn(L, intern(q), n->k.n);                 /* instance method: + this */
            if (f && !f->is_static) {
                Sym *th = lookup(L, intern("this"));
                if (!th) {
                    diag(L->ds, SEV_ERROR, "semantic", "Static Context", n->line, n->col, "Make the method static, or call it on an object.",
                         "non-static method %s() cannot be referenced from a static context", name);
                    return opaque(L, n, "an instance method call from a static method", want);
                }
                Opnd recv = sym_opnd(th);
                return user_call(L, n, f, 1, &recv, want);
            }
            if (f) return user_call(L, n, f, 1, NULL, want);
        }
        FnSig *f = find_fn(L, name, n->k.n - 1);
        Sym *s = lookup(L, name);
        if (f && (!s || s->kind == SK_FUNC)) { if (s) use_sym(L, s); return user_call(L, n, f, 1, NULL, want); }
        if (s && s->kind == SK_MACRO) return opaque(L, n, "a call to a function-like macro", want);
        if (s && s->kind == SK_CLASS) {
            for (int i = 1; i < n->k.n; i++) args[na++] = gen(L, n->k.v[i]);
            push_args(L, args, na);
            L->P->external_calls++;
            return put_call(L, name, na, L->lang == LANG_PY ? TY_DYN : TY_OBJ, want);
        }
        if (s && (s->kind == SK_VAR || s->kind == SK_PARAM || s->kind == SK_GLOBAL)) return opaque(L, n, "a call through a variable (function value)", want);
        /* library / builtin call; Python keyword arguments become part of the name: print:sep:end */
        Buf fn = {0};
        bputs(&fn, name);
        for (int i = 1; i < n->k.n && na < 64; i++) {
            Node *a = n->k.v[i];
            if (a->kind == N_Star) { free(fn.s); return opaque(L, n, "a call with *args unpacking", want); }
            if (a->kind == N_KwArg) continue;
            args[na++] = gen(L, a);
        }
        for (int i = 1; i < n->k.n && na < 64; i++) {
            Node *a = n->k.v[i];
            if (a->kind != N_KwArg) continue;
            bprintf(&fn, ":%s", a->s);
            args[na++] = gen(L, a->k.v[0]);
        }
        int known = L->lang == LANG_PY ? in_list(name, py_builtins) : L->lang == LANG_C ? in_list(name, c_known) : 0;
        char hint[128];
        if (!known) {
            if (L->lang == LANG_C) diag(L->ds, SEV_WARNING, "semantic", "Implicit Declaration", callee->line, callee->col,
                                        suggest(L, name, hint, sizeof(hint), "Declare or define the function before calling it."), "implicit declaration of function '%s'", name);
            else if (L->lang == LANG_JAVA) diag(L->ds, SEV_ERROR, "semantic", "Undeclared Method", callee->line, callee->col, NULL, "cannot find method '%s' in class %s", name, L->cls ? L->cls : "?");
            else diag(L->ds, SEV_WARNING, "semantic", "Undefined Name", callee->line, callee->col, suggest(L, name, hint, sizeof(hint), NULL), "function '%s' is not defined in this file", name);
            L->P->external_calls++;
        }
        if (L->lang == LANG_C && !strcmp(name, "printf") && na && args[0].k != OP_STR)
            diag(L->ds, SEV_WARNING, "semantic", "Format Warning", n->line, n->col, "Use a string literal as the format.", "printf format is not a string literal");
        if (L->lang == LANG_C && (!strcmp(name, "malloc") || !strcmp(name, "calloc") || !strcmp(name, "realloc") || !strcmp(name, "free")))
            { free(fn.s); return opaque(L, n, "dynamic memory management (malloc/free)", want); }
        push_args(L, args, na);
        Opnd r = put_call(L, fn.s, na, lib_ret(L, fn.s, args, na), want);
        free(fn.s);
        return r;
    }

    if (callee->kind == N_Member) {
        Node *obj = callee->k.v[0];
        Buf d = {0};
        const char *chain = dotted(callee, &d);
        Opnd k;
        /* Class.method / module.function / System.out.println */
        int qualified = 0;
        if (chain) {
            const char *first = intern_n(chain, strcspn(chain, "."));
            Sym *s = lookup(L, first);
            if (L->lang == LANG_PY) qualified = s ? s->kind == SK_MODULE || s->kind == SK_CLASS : 0;
            else if (L->lang == LANG_JAVA) qualified = (!s || s->kind == SK_CLASS) && (isupper((unsigned char)chain[0]) || !strncmp(chain, "java.", 5));
            else qualified = 0;
            if (qualified && L->lang == LANG_JAVA) {
                FnSig *f = find_fn(L, intern(chain), n->k.n - 1);
                if (f) { free(d.s); return user_call(L, n, f, 1, NULL, want); }
            }
            if (qualified && L->lang == LANG_PY && s && s->kind == SK_CLASS) {
                FnSig *f = find_fn(L, intern(chain), -1);
                if (f) { free(d.s); return user_call(L, n, f, 1, NULL, want); }
            }
        }
        (void)k;
        if (qualified) {
            for (int i = 1; i < n->k.n && na < 64; i++) {
                if (n->k.v[i]->kind == N_KwArg || n->k.v[i]->kind == N_Star) { free(d.s); return opaque(L, n, "a library call with keyword arguments", want); }
                args[na++] = gen(L, n->k.v[i]);
            }
            push_args(L, args, na);
            L->P->external_calls++;
            Opnd r = put_call(L, chain, na, lib_ret(L, chain, args, na), want);
            free(d.s);
            return r;
        }
        free(d.s);
        /* method call on a value: the receiver is the first parameter */
        Opnd recv = gen(L, obj);
        const char *m = callee->s;
        if (L->lang == LANG_JAVA && recv.ty == TY_STR && !strcmp(m, "length") && n->k.n == 1) return put_un(L, U_LEN, recv, TY_INT);
        if (L->lang == LANG_JAVA && recv.ty == TY_STR && !strcmp(m, "charAt") && n->k.n == 2) {
            Opnd idx = gen(L, n->k.v[1]);
            Opnd dd = temp(L, TY_CHAR);
            Instr *i = emit(L, I_LOAD);
            i->d = dd; i->a = recv; i->b = idx; i->ty = TY_CHAR;
            return dd;
        }
        if (L->lang == LANG_PY && obj->kind == N_Name && (!strcmp(obj->s, "self") || !strcmp(obj->s, "cls")) && L->cls) {
            char q[256]; snprintf(q, sizeof(q), "%s.%s", L->cls, m);
            FnSig *f = find_fn(L, intern(q), -1);
            if (f) return user_call(L, n, f, 1, &recv, want);
        }
        if (L->lang == LANG_C) return opaque(L, n, "a call through a struct member", want);
        args[na++] = recv;
        for (int i = 1; i < n->k.n && na < 64; i++) {
            if (n->k.v[i]->kind == N_KwArg || n->k.v[i]->kind == N_Star) return opaque(L, n, "a method call with keyword arguments", want);
            args[na++] = gen(L, n->k.v[i]);
        }
        push_args(L, args, na);
        char fn[160];
        if (L->lang == LANG_JAVA) snprintf(fn, sizeof(fn), "%s.%s", recv.ty == TY_STR ? "String" : recv.ty == TY_ARR ? "array" : "Object", m);
        else snprintf(fn, sizeof(fn), ".%s", m);
        Ty rt = lib_ret(L, fn, args, na);
        if (L->lang == LANG_JAVA && rt == TY_UNKNOWN && recv.ty != TY_STR) L->P->external_calls++;
        return put_call(L, fn, na, rt, want);
    }
    return opaque(L, n, "a call to a computed function value", want);
}

/* ---- the expression lowering proper ---- */

static Opnd logical_value(Lw *L, Node *n) {
    /* C/Java: 0/1 or boolean.  Python: the deciding operand's own value */
    int is_and = n->s[0] == '&' || n->s[0] == 'a';
    if (L->lang == LANG_PY) {
        Opnd t = temp(L, TY_DYN);
        int end = label(L);
        put_copy(L, t, gen(L, n->k.v[0]));
        put_if(L, t, end, !is_and);
        put_copy(L, t, gen(L, n->k.v[1]));
        put_label(L, end);
        return t;
    }
    Ty rt = L->lang == LANG_JAVA ? TY_BOOL : TY_INT;
    Opnd t = temp(L, rt);
    int f = label(L), end = label(L);
    branch_false(L, n, f);
    put_copy(L, t, L->lang == LANG_JAVA ? o_bool(1) : o_int(1, TY_INT));
    put_goto(L, end);
    put_label(L, f);
    put_copy(L, t, L->lang == LANG_JAVA ? o_bool(0) : o_int(0, TY_INT));
    put_label(L, end);
    return t;
}

static Opnd seq_literal(Lw *L, Node *n, int kind, Ty elem, Opnd *shape) {
    /* kind: 0 C/Java array, 1 Python list, 2 Python tuple */
    for (int i = 0; i < n->k.n; i++)
        if (n->k.v[i]->kind == N_Star) return opaque(L, n, "unpacking (*x) inside a list", 1);
    Opnd d = temp(L, kind ? TY_DYN : TY_ARR);
    if (!kind) { d.el = shape ? shape->el : (unsigned char)elem; d.rank = shape ? shape->rank : 1; }
    Instr *in = emit(L, I_NEWARR);
    in->d = d; in->a = o_int(n->k.n, TY_INT); in->c = o_int(kind, TY_INT); in->ty = (unsigned char)(kind ? TY_DYN : (d.rank > 1 ? TY_ARR : elem));
    for (int i = 0; i < n->k.n; i++) {
        Opnd v;
        if (n->k.v[i]->kind == N_ArrayInit && !kind) {
            Opnd sub = o_none(); sub.el = d.el; sub.rank = (unsigned char)(d.rank > 1 ? d.rank - 1 : 1);
            v = seq_literal(L, n->k.v[i], 0, (Ty)d.el, &sub);
        } else {
            v = gen(L, n->k.v[i]);
            if (!kind) v = assign_conv(L, n->k.v[i], v, d.rank > 1 ? TY_ARR : elem, "an array initializer");
        }
        Instr *s = emit(L, I_STORE);
        s->a = d; s->b = o_int(i, TY_INT); s->c = v; s->ty = (unsigned char)(kind ? TY_DYN : elem);
    }
    return d;
}

static Opnd fstring(Lw *L, Node *n) {
    Opnd acc = o_none();
    for (int i = 0; i < n->k.n; i++) {
        Node *p = n->k.v[i];
        Opnd part;
        if (p->kind == N_StrLit) part = o_str(p->s);
        else {
            Opnd v = gen(L, p->k.v[0]);
            if (p->s) { put_param(L, v); put_param(L, o_str(p->s)); part = put_call(L, "format", 2, TY_DYN, 1); }
            else part = put_un(L, p->type ? U_REPR : U_TOSTR, v, TY_DYN);
        }
        acc = acc.k ? put_bin(L, B_ADD, acc, part, TY_DYN, TY_DYN) : part;
    }
    return acc.k ? acc : o_str(intern(""));
}

static Opnd gen(Lw *L, Node *n) {
    int saved_line = L->line;
    if (n->line) L->line = n->line;
    Opnd r;
    switch (n->kind) {
    case N_IntLit: r = lit_int(L, n); break;
    case N_FloatLit: {
        size_t len = n->s ? strlen(n->s) : 0;
        int f32 = L->lang != LANG_PY && len && (n->s[len - 1] == 'f' || n->s[len - 1] == 'F');
        r = o_flt(f32 ? (float)n->fval : n->fval, f32 ? TY_FLOAT : TY_DOUBLE);
        break;
    }
    case N_StrLit: r = o_str(n->s); break;
    case N_CharLit: r = o_int(n->ival, TY_CHAR); break;
    case N_BoolLit: r = L->lang == LANG_C ? o_int(n->ival, TY_INT) : o_bool((int)n->ival); break;
    case N_NullLit: r = o_null(); break;
    case N_Name: r = name_ref(L, n); break;
    case N_Binary: {
        if (!strcmp(n->s, ",")) { gen(L, n->k.v[0]); r = gen(L, n->k.v[1]); break; }
        BinOp op;
        if (!binop_of(n->s, &op)) { r = opaque(L, n, "this operator", 1); break; }
        if (!strcmp(n->s, "@")) { r = opaque(L, n, "matrix multiplication (@)", 1); break; }
        Opnd a = gen(L, n->k.v[0]);
        Opnd b = gen(L, n->k.v[1]);
        r = binary_op(L, n, op, a, b);
        break;
    }
    case N_Compare: {
        /* Python chain a < b < c: each middle operand evaluated once */
        char ops[256];
        snprintf(ops, sizeof(ops), "%s", n->s);
        char *save = NULL, *tok = strtok_r(ops, ",", &save);
        Opnd left = gen(L, n->k.v[0]);
        if (n->k.n == 2) {
            BinOp op; binop_of(tok, &op);
            r = put_bin(L, op, left, gen(L, n->k.v[1]), TY_DYN, TY_DYN);
            break;
        }
        Opnd res = temp(L, TY_DYN);
        int end = label(L);
        for (int i = 1; i < n->k.n; i++) {
            BinOp op; binop_of(tok, &op);
            Opnd right = gen(L, n->k.v[i]);
            if (i < n->k.n - 1 && right.k != OP_TEMP && !o_const(&right)) { Opnd t = temp(L, TY_DYN); put_copy(L, t, right); right = t; }
            Opnd c = put_bin(L, op, left, right, TY_DYN, TY_DYN);
            put_copy(L, res, c);
            if (i < n->k.n - 1) put_if(L, res, end, 0);
            left = right;
            tok = strtok_r(NULL, ",", &save);
        }
        put_label(L, end);
        r = res;
        break;
    }
    case N_Logical: r = logical_value(L, n); break;
    case N_Unary: {
        const char *op = n->s;
        if (!strcmp(op, "&")) { r = opaque(L, n, "taking an address (&)", 1); break; }
        if (!strcmp(op, "*")) {
            LVal lv = lval(L, n, 0);
            r = lval_load(L, &lv);
            break;
        }
        Opnd a = gen(L, n->k.v[0]);
        if (!strcmp(op, "!") || !strcmp(op, "not")) {
            if (L->lang == LANG_JAVA && a.ty != TY_BOOL && a.ty != TY_UNKNOWN)
                diag(L->ds, SEV_ERROR, "semantic", "Type Error", n->line, n->col, NULL, "operator ! needs a boolean, not %s", src_ty(L, (Ty)a.ty));
            r = put_un(L, U_NOT, a, L->lang == LANG_C ? TY_INT : L->lang == LANG_JAVA ? TY_BOOL : TY_DYN);
            break;
        }
        if (a.k == OP_INT && L->lang != LANG_PY && !strcmp(op, "-") && a.i != INT_MIN) { a.i = -a.i; if (a.ty == TY_CHAR || a.ty == TY_BOOL) a.ty = TY_INT; r = a; break; }
        if (a.k == OP_INT && L->lang == LANG_PY && !strcmp(op, "-")) { a.i = -a.i; r = a; break; }
        if (a.k == OP_FLT && !strcmp(op, "-")) { a.f = -a.f; r = a; break; }
        Ty t = L->lang == LANG_PY ? TY_DYN : arith_ty(L, (Ty)a.ty, TY_INT);
        if (L->lang != LANG_PY) a = convert(L, a, t);
        r = put_un(L, !strcmp(op, "-") ? U_NEG : !strcmp(op, "~") ? U_BNOT : U_POS, a, t);
        break;
    }
    case N_IncDec: {
        LVal lv = lval(L, n->k.v[0], 0);
        if (lv.kind == 2) { r = opaque(L, n, "++/-- on a field or member", 1); break; }
        Opnd old = lval_load(L, &lv);
        Ty t = lval_ty(L, &lv);
        Opnd one = (t == TY_DOUBLE || t == TY_FLOAT) ? o_flt(1, t) : o_int(1, TY_INT);
        if (!n->ival && lv.kind == 0) { Opnd keep = temp(L, t); put_copy(L, keep, old); old = keep; }
        Ty at = arith_ty(L, t, TY_INT);
        Opnd nv = put_bin(L, n->s[0] == '+' ? B_ADD : B_SUB, convert(L, old, at), one, at, at);
        nv = convert(L, nv, t);
        lval_store(L, &lv, nv);
        r = n->ival ? nv : old;
        break;
    }
    case N_Assign: {
        /* targets..., value (Python chains a = b = v); compound ops read-modify-write */
        int nt = n->k.n - 1;
        Node *vnode = n->k.v[nt];
        if (strcmp(n->s, "=") != 0) {
            BinOp op;
            char opstr[8];
            snprintf(opstr, sizeof(opstr), "%.*s", (int)strlen(n->s) - 1, n->s);
            if (!binop_of(opstr, &op) || !strcmp(opstr, "@")) { r = opaque(L, n, "this compound assignment operator", 1); break; }
            LVal lv = lval(L, n->k.v[0], 0);
            if (lv.kind == 2) { r = opaque(L, n, "compound assignment to a field or member", 1); break; }
            Opnd cur = lval_load(L, &lv);
            Opnd v = gen(L, vnode);
            Ty t = lval_ty(L, &lv);
            Opnd res = binary_op(L, n, op, cur, v);
            res = convert(L, res, t);                    /* compound assignment casts implicitly */
            lval_store(L, &lv, res);
            r = res;
            break;
        }
        if (nt == 1 && (n->k.v[0]->kind == N_Tuple || n->k.v[0]->kind == N_ArrayInit) && L->lang == LANG_PY) {
            /* a, b = x, y : evaluate everything on the right first */
            Node *tgt = n->k.v[0];
            Opnd vals[64];
            int nv = 0;
            if (vnode->kind == N_Tuple && vnode->k.n == tgt->k.n) {
                for (int i = 0; i < vnode->k.n && nv < 64; i++) {
                    Opnd v = gen(L, vnode->k.v[i]);
                    if (v.k == OP_VAR) { Opnd t = temp(L, TY_DYN); put_copy(L, t, v); v = t; }
                    vals[nv++] = v;
                }
            } else {
                Opnd whole = gen(L, vnode);
                for (int i = 0; i < tgt->k.n && nv < 64; i++) {
                    Opnd d = temp(L, TY_DYN);
                    Instr *in = emit(L, I_LOAD);
                    in->d = d; in->a = whole; in->b = o_int(i, TY_INT); in->ty = TY_DYN;
                    vals[nv++] = d;
                }
            }
            for (int i = 0; i < tgt->k.n && i < nv; i++) {
                if (tgt->k.v[i]->kind == N_Star || tgt->k.v[i]->kind == N_Tuple) { opaque(L, tgt->k.v[i], "nested or starred unpacking", 0); continue; }
                LVal lv = lval(L, tgt->k.v[i], 1);
                lval_store(L, &lv, vals[i]);
            }
            r = o_none();
            break;
        }
        Opnd v;
        if (vnode->kind == N_ArrayInit && L->lang != LANG_PY) {
            LVal lv0 = lval(L, n->k.v[0], 1);
            Opnd shape = lv0.kind == 0 ? lv0.var : o_none();
            v = seq_literal(L, vnode, 0, shape.ty == TY_ARR ? (Ty)shape.el : TY_INT, shape.ty == TY_ARR ? &shape : NULL);
        } else v = gen(L, vnode);
        if (L->lang == LANG_PY && nt > 1 && v.k == OP_TEMP) { /* fine: a temp can be copied to several targets */ }
        for (int i = 0; i < nt; i++) {
            Node *tn = n->k.v[i];
            if (tn->kind == N_Tuple || tn->kind == N_ArrayInit) { opaque(L, tn, "unpacking assignment in a chain", 0); continue; }
            LVal lv = lval(L, tn, 1);
            Opnd cv = assign_conv(L, n, v, lval_ty(L, &lv), "an assignment");
            lval_store(L, &lv, cv);
            r = lv.kind == 0 ? lv.var : cv;
        }
        if (nt == 0) r = v;
        break;
    }
    case N_Ternary: {
        int f = label(L), end = label(L);
        branch_false(L, n->k.v[0], f);
        Opnd a = gen(L, n->k.v[1]);
        Ty t = L->lang == LANG_PY ? TY_DYN : (ty_is_num((Ty)a.ty) ? (Ty)a.ty : (Ty)a.ty);
        Opnd res = temp(L, t);
        res.el = a.el; res.rank = a.rank;
        int copy_idx = L->fn->code.n;
        put_copy(L, res, a);
        put_goto(L, end);
        put_label(L, f);
        Opnd b = gen(L, n->k.v[2]);
        if (L->lang != LANG_PY && ty_is_num((Ty)a.ty) && ty_is_num((Ty)b.ty) && a.ty != b.ty) {
            Ty u = arith_ty(L, (Ty)a.ty, (Ty)b.ty);
            res.ty = (unsigned char)u;
            L->fn->code.v[copy_idx].d.ty = (unsigned char)u;
            L->fn->code.v[copy_idx].ty = (unsigned char)u;
            if (a.k == OP_INT && (u == TY_DOUBLE || u == TY_FLOAT)) L->fn->code.v[copy_idx].a = o_flt((double)a.i, u);
            b = convert(L, b, u);
        }
        put_copy(L, res, b);
        put_label(L, end);
        r = res;
        break;
    }
    case N_Call: r = call_expr(L, n, 1); break;
    case N_Index: {
        if (n->k.v[1]->kind == N_Slice) {
            Node *s = n->k.v[1];
            Opnd base = gen(L, n->k.v[0]);
            Opnd p[3];
            for (int i = 0; i < 3; i++) p[i] = s->k.v[i]->kind == N_Empty ? o_null() : gen(L, s->k.v[i]);
            put_param(L, base);
            for (int i = 0; i < 3; i++) put_param(L, p[i]);
            r = put_call(L, "slice", 4, TY_DYN, 1);
            break;
        }
        if (n->k.v[1]->kind == N_Tuple) { r = opaque(L, n, "multi-dimensional subscript a[i, j]", 1); break; }
        LVal lv = lval(L, n, 0);
        if (L->lang == LANG_JAVA && lv.idx.ty != TY_UNKNOWN && !ty_is_int((Ty)lv.idx.ty))
            diag(L->ds, SEV_ERROR, "semantic", "Type Error", n->line, n->col, NULL, "array index must be an integer, not %s", src_ty(L, (Ty)lv.idx.ty));
        if (L->lang != LANG_PY && lv.base.ty != TY_ARR && lv.base.ty != TY_STR && lv.base.ty != TY_UNKNOWN && lv.base.ty != TY_OBJ)
            diag(L->ds, SEV_ERROR, "semantic", "Type Error", n->line, n->col, NULL, "subscripted value is not an array (it is %s)", src_ty(L, (Ty)lv.base.ty));
        r = lval_load(L, &lv);
        break;
    }
    case N_Member: {
        Buf d = {0};
        const char *chain = dotted(n, &d);
        Opnd k;
        if (chain && named_constant(L, chain, &k)) { free(d.s); r = k; break; }
        if (chain && L->lang == LANG_JAVA) {
            Sym *s = lookup(L, intern(chain));
            if (s && (s->kind == SK_GLOBAL || s->kind == SK_CONST)) { use_sym(L, s); free(d.s); r = sym_opnd(s); break; }
        }
        free(d.s);
        if (!strcmp(n->s, "length") && L->lang == LANG_JAVA) {
            Opnd base = gen(L, n->k.v[0]);
            if (base.ty == TY_ARR || base.ty == TY_UNKNOWN) { r = put_un(L, U_LEN, base, TY_INT); break; }
        }
        if (L->lang == LANG_PY && n->k.v[0]->kind == N_Name && is_module(L, n->k.v[0]->s)) {
            char q[128]; snprintf(q, sizeof(q), "%s.%s", n->k.v[0]->s, n->s);
            r = put_call(L, q, 0, TY_DYN, 1);
            break;
        }
        char what[96];
        snprintf(what, sizeof(what), "field access .%s", n->s);
        if (n->k.v[0]->kind != N_Name) gen(L, n->k.v[0]);
        r = opaque(L, n, what, 1);
        break;
    }
    case N_Cast: {
        Opnd a = gen(L, n->k.v[0]);
        Ty t, el; int rank;
        type_of(L, n->type, &t, &el, &rank);
        if (t == TY_ARR || t == TY_STR || t == TY_OBJ || t == TY_UNKNOWN) { r = a; break; }
        if (L->lang == LANG_JAVA && (a.ty == TY_BOOL) != (t == TY_BOOL) && a.ty != TY_UNKNOWN)
            diag(L->ds, SEV_ERROR, "semantic", "Type Error", n->line, n->col, NULL, "incompatible types: %s cannot be cast to %s", src_ty(L, (Ty)a.ty), src_ty(L, t));
        if (n->type && (strstr(n->type, "short") || strstr(n->type, "byte"))) { r = opaque(L, n, "a cast to short/byte", 1); break; }
        r = convert(L, a, t);
        if (r.k != OP_TEMP || r.ty != t) {
            if (o_const(&r)) break;
            Opnd c = temp(L, t);
            put_copy(L, c, r);
            r = c;
        }
        break;
    }
    case N_NewArray: {
        Ty el = base_ty(L->lang, n->type);
        if (n->k.n == 1 && n->k.v[0]->kind == N_ArrayInit) {
            Opnd shape = o_none(); shape.el = (unsigned char)el; shape.rank = (unsigned char)(n->ival ? n->ival : 1);
            r = seq_literal(L, n->k.v[0], 0, el, &shape);
            break;
        }
        Opnd d = temp(L, TY_ARR);
        d.el = (unsigned char)el; d.rank = (unsigned char)n->ival;
        Opnd dims[2] = { o_none(), o_none() };
        int nd = 0;
        for (int i = 0; i < n->k.n && i < 3; i++) {
            if (n->k.v[i]->kind == N_Empty) break;
            if (i >= 2) { r = opaque(L, n, "an array with more than two allocated dimensions", 1); goto done; }
            dims[nd++] = gen(L, n->k.v[i]);
        }
        Instr *in = emit(L, I_NEWARR);
        in->d = d; in->a = dims[0]; in->b = nd > 1 ? dims[1] : o_none();
        in->c = o_int(0, TY_INT);
        in->ty = (unsigned char)(n->ival > nd ? TY_ARR : el);
        r = d;
        break;
    }
    case N_NewObject: {
        Opnd args[64]; int na = 0;
        for (int i = 0; i < n->k.n && na < 64; i++) args[na++] = gen(L, n->k.v[i]);
        push_args(L, args, na);
        char fn[160]; snprintf(fn, sizeof(fn), "new %s", n->s);
        L->P->external_calls++;
        r = put_call(L, fn, na, !strcmp(n->s, "String") ? TY_STR : TY_OBJ, 1);
        break;
    }
    case N_ArrayInit:
        r = L->lang == LANG_PY ? seq_literal(L, n, 1, TY_DYN, NULL) : seq_literal(L, n, 0, TY_INT, NULL);
        break;
    case N_Tuple: r = seq_literal(L, n, 2, TY_DYN, NULL); break;
    case N_FString: r = fstring(L, n); break;
    case N_Sizeof: {
        static const struct { const char *t; int n; } sz[] = { {"char", 1}, {"short", 2}, {"int", 4}, {"unsigned", 4},
            {"long long", 8}, {"long", 8}, {"float", 4}, {"double", 8}, {"size_t", 8} };
        if (n->type) {
            if (strchr(n->type, '*')) { r = o_int(8, TY_LONG); break; }
            int found = 0;
            for (size_t i = 0; i < sizeof(sz) / sizeof(sz[0]); i++)
                if (strstr(n->type, sz[i].t)) { r = o_int(sz[i].n, TY_LONG); found = 1; break; }
            if (found) break;
        }
        r = opaque(L, n, "sizeof of a non-primitive type", 1);
        break;
    }
    case N_Dict: r = opaque(L, n, n->s ? "a set literal" : "a dict literal", 1); break;
    case N_Lambda: r = opaque(L, n, "a lambda expression", 1); break;
    case N_Comprehension: r = opaque(L, n, "a comprehension", 1); break;
    case N_Unsupported: r = opaque(L, n, n->s ? n->s : "an unsupported construct", 1); break;
    default: {
        char what[64]; snprintf(what, sizeof(what), "a %s expression", node_kind_name(n->kind));
        r = opaque(L, n, what, 1);
    }
    }
done:
    L->line = saved_line;
    return r;
}

/* ---- conditions as jumps ---- */

static int is_not(Node *c) { return c->kind == N_Unary && c->s && (!strcmp(c->s, "!") || !strcmp(c->s, "not")); }
static int is_and(Node *c) { return c->kind == N_Logical && (c->s[0] == '&' || c->s[0] == 'a'); }
static int is_or(Node *c) { return c->kind == N_Logical && (c->s[0] == '|' || c->s[0] == 'o'); }

static void branch_false(Lw *L, Node *c, int lab) {
    if (is_not(c)) { branch_true(L, c->k.v[0], lab); return; }
    if (is_and(c)) { branch_false(L, c->k.v[0], lab); branch_false(L, c->k.v[1], lab); return; }
    if (is_or(c)) {
        int ok = label(L);
        branch_true(L, c->k.v[0], ok);
        branch_false(L, c->k.v[1], lab);
        put_label(L, ok);
        return;
    }
    Opnd v = gen(L, c);
    if (L->lang == LANG_JAVA && v.ty != TY_BOOL && v.ty != TY_UNKNOWN)
        diag(L->ds, SEV_ERROR, "semantic", "Type Error", c->line, c->col, "Use a comparison, e.g.  x != 0", "condition must be boolean, not %s", src_ty(L, (Ty)v.ty));
    put_if(L, v, lab, 0);
}

static void branch_true(Lw *L, Node *c, int lab) {
    if (is_not(c)) { branch_false(L, c->k.v[0], lab); return; }
    if (is_or(c)) { branch_true(L, c->k.v[0], lab); branch_true(L, c->k.v[1], lab); return; }
    if (is_and(c)) {
        int no = label(L);
        branch_false(L, c->k.v[0], no);
        branch_true(L, c->k.v[1], lab);
        put_label(L, no);
        return;
    }
    Opnd v = gen(L, c);
    put_if(L, v, lab, 1);
}

/* ---------------------------------------------------------------------
 * Statements
 * ------------------------------------------------------------------- */

static Sym *declare(Lw *L, Node *at, const char *name, Ty ty, Ty el, int rank, int kind, const char *type_text) {
    int base = L->marks.n ? L->marks.v[L->marks.n - 1] : 0;
    for (int i = base; i < L->syms.n; i++)
        if (L->syms.v[i].name == name) {
            if (L->lang != LANG_PY)
                diag(L->ds, SEV_ERROR, "semantic", "Duplicate Declaration", at->line, at->col, "Rename one of the variables.",
                     "'%s' is already declared in this scope (line %d)", name, L->syms.v[i].line);
            return &L->syms.v[i];
        }
    /* shadowing an outer local: give the inner one a distinct IR name */
    const char *ir = name;
    for (int i = 0; i < base; i++)
        if (L->syms.v[i].name == name) {
            char buf[160];
            for (int k = 1; ; k++) {
                snprintf(buf, sizeof(buf), "%s.%d", name, k);
                const char *cand = intern(buf);
                int used = 0;
                for (int j = 0; j < L->syms.n; j++) if (L->syms.v[j].ir == cand) used = 1;
                if (!used) { ir = cand; break; }
            }
            break;
        }
    Sym s = {0};
    s.name = name; s.ir = ir; s.ty = (unsigned char)ty; s.el = (unsigned char)el; s.rank = (unsigned char)rank;
    s.kind = (unsigned char)kind; s.line = at->line; s.col = at->col;
    s.info = record_sym(L, name, kind, type_text, at->line, at->col);
    vpush(L->syms, s);
    return &L->syms.v[L->syms.n - 1];
}

static void var_decl(Lw *L, Node *d, int global) {
    if (d->kind != N_VarDecl) { if (d->kind == N_Unsupported) opaque(L, d, d->s, 0); return; }
    if (!d->s) return;
    Ty ty, el; int rank;
    type_of(L, d->type, &ty, &el, &rank);
    Node *init = d->k.v[0];
    /* C: char buf[N] without a string initializer is an array of chars */
    if (L->lang == LANG_C && ty == TY_STR && d->ival > 0 && (init->kind != N_StrLit)) { ty = TY_ARR; el = TY_CHAR; rank = (int)d->ival; }
    if (ty == TY_UNKNOWN && init->kind != N_Empty) {      /* Java 'var' */
        Opnd v = gen(L, init);
        Sym *s = declare(L, d, d->s, (Ty)v.ty, (Ty)v.el, v.rank, global ? SK_GLOBAL : SK_VAR, ty_name((Ty)v.ty));
        s->global = (unsigned char)global;
        put_copy(L, sym_opnd(s), v);
        return;
    }
    Sym *s;
    if (global) {
        s = lookup(L, d->s);
        if (!s) return;
    } else s = declare(L, d, d->s, ty, el, rank, SK_VAR, d->type);
    Opnd var = sym_opnd(s);
    if (init->kind == N_ArrayInit) {
        Opnd arr = seq_literal(L, init, 0, el, &var);
        if (L->lang == LANG_C && d->k.n > 1 && d->k.v[1]->kind == N_IntLit && d->k.v[1]->ival > init->k.n) {
            /* int a[10] = {1, 2}: the rest are zero */
            Instr *nw = NULL;
            for (int i = L->fn->code.n - 1; i >= 0; i--) if (L->fn->code.v[i].op == I_NEWARR && o_same(&L->fn->code.v[i].d, &arr)) { nw = &L->fn->code.v[i]; break; }
            if (nw) nw->a = o_int(d->k.v[1]->ival, TY_INT);
        }
        put_copy(L, var, arr);
    } else if (init->kind == N_StrLit && L->lang == LANG_C && ty == TY_ARR) {
        put_copy(L, var, o_str(init->s));
        s->ty = TY_STR;
    } else if (init->kind != N_Empty) {
        Opnd v = gen(L, init);
        if (L->lang == LANG_JAVA && ty == TY_CHAR && v.k == OP_INT && v.ty == TY_INT && v.i >= 0 && v.i <= 0xFFFF) v.ty = TY_CHAR;
        if ((d->flags & NF_POINTER) && L->lang == LANG_C && v.ty == TY_ARR) { var.el = v.el; }
        put_copy(L, var, assign_conv(L, d, v, ty, "the initialization"));
    } else if (rank > 0 && d->k.n > 1 && d->k.v[1]->kind != N_Empty && L->lang == LANG_C) {
        /* C local array with a size: allocate it */
        Opnd n0 = gen(L, d->k.v[1]);
        Opnd n1 = d->k.n > 2 && d->k.v[2]->kind != N_Empty ? gen(L, d->k.v[2]) : o_none();
        if (d->k.n > 3) { opaque(L, d, "an array with more than two dimensions", 0); return; }
        Instr *in = emit(L, I_NEWARR);
        in->d = var; in->a = n0; in->b = n1; in->c = o_int(0, TY_INT); in->ty = (unsigned char)(rank > 1 && !n1.k ? TY_ARR : el);
    } else if (global || L->lang == LANG_JAVA) {
        /* globals and Java fields start at zero / null / false */
        if (ty == TY_DOUBLE || ty == TY_FLOAT) put_copy(L, var, o_flt(0, ty));
        else if (ty == TY_BOOL && L->lang == LANG_JAVA) put_copy(L, var, o_bool(0));
        else if (ty_is_int(ty)) put_copy(L, var, o_int(0, ty));
        else if (global) put_copy(L, var, o_null());
    }
}

static int count_instrs(Lw *L, int from) {
    int c = 0;
    for (int i = from; i < L->fn->code.n; i++) c += L->fn->code.v[i].op != I_LABEL;
    return c;
}

static void trace_stmt(Lw *L, Node *n, int from) {
    if (L->info->trace.n >= 300) return;
    char buf[200];
    snprintf(buf, sizeof(buf), "line %d: %s in %s -> %d TAC instruction%s", n->line, node_kind_name(n->kind),
             L->fn->name, count_instrs(L, from), count_instrs(L, from) == 1 ? "" : "s");
    vpush(L->info->trace, xstrdup(buf));
}

static void loop_push(Lw *L, int brk, int cont) { vpush(L->brk, brk); vpush(L->cont, cont); }
static void loop_pop(Lw *L) { L->brk.n--; L->cont.n--; }

/* Python for-loops over range(), enumerate() and sequences become index loops */
static void py_for(Lw *L, Node *n) {
    Node *iter = n->k.v[0], *body = n->k.v[1], *els = n->k.v[2];
    Node *tuple_target = n->ival ? n->k.v[3] : NULL;
    int lcond = label(L), lcont = label(L), lend = label(L), lelse = label(L);
    LVal target;
    memset(&target, 0, sizeof(target));
    Node fake;
    memset(&fake, 0, sizeof(fake));
    fake.kind = N_Name; fake.s = n->s; fake.line = n->line; fake.col = n->col;

    if (iter->kind == N_Call && iter->k.v[0]->kind == N_Name && !strcmp(iter->k.v[0]->s, "range") &&
        !lookup(L, intern("range")) && iter->k.n >= 2 && iter->k.n <= 4 && !tuple_target) {
        Opnd a[3];
        int na = iter->k.n - 1;
        for (int i = 0; i < na; i++) a[i] = gen(L, iter->k.v[i + 1]);
        Opnd start = na == 1 ? o_int(0, TY_INT) : a[0];
        Opnd stop = na == 1 ? a[0] : a[1];
        Opnd step = na == 3 ? a[2] : o_int(1, TY_INT);
        int all_const = (start.k == OP_INT) && (stop.k == OP_INT) && (step.k == OP_INT);
        if (!all_const) {                         /* range() itself rejects non-integers and step 0 */
            put_param(L, start); put_param(L, stop); put_param(L, step);
            put_call(L, "range_check", 3, TY_VOID, 0);
        }
        if (step.k == OP_INT && step.i == 0) diag(L->ds, SEV_ERROR, "semantic", "Value Error", iter->line, iter->col, NULL, "range() arg 3 must not be zero");
        if (stop.k == OP_VAR) { Opnd t = temp(L, TY_DYN); put_copy(L, t, stop); stop = t; }
        if (step.k == OP_VAR) { Opnd t = temp(L, TY_DYN); put_copy(L, t, step); step = t; }
        Opnd i = temp(L, TY_INT);
        Instr *ci = emit(L, I_COPY);
        ci->d = i; ci->a = start; ci->ty = TY_INT;          /* ty INT: the counter is a proven int */
        put_label(L, lcond);
        Opnd c;
        if (step.k == OP_INT) c = put_bin(L, step.i > 0 ? B_LT : B_GT, i, stop, TY_DYN, TY_DYN);
        else {
            int neg = label(L), test = label(L);
            c = temp(L, TY_DYN);
            Opnd pos = put_bin(L, B_GT, step, o_int(0, TY_INT), TY_DYN, TY_DYN);
            put_if(L, pos, neg, 0);
            put_copy(L, c, put_bin(L, B_LT, i, stop, TY_DYN, TY_DYN));
            put_goto(L, test);
            put_label(L, neg);
            put_copy(L, c, put_bin(L, B_GT, i, stop, TY_DYN, TY_DYN));
            put_label(L, test);
        }
        put_if(L, c, lelse, 0);
        target = lval(L, &fake, 1);
        lval_store(L, &target, i);
        loop_push(L, lend, lcont);
        stmt(L, body);
        loop_pop(L);
        put_label(L, lcont);
        Opnd nx = put_bin(L, B_ADD, i, step, TY_INT, TY_DYN);
        Instr *inc = emit(L, I_COPY);
        inc->d = i; inc->a = nx; inc->ty = TY_INT;
        put_goto(L, lcond);
    } else {
        int is_enum = iter->kind == N_Call && iter->k.v[0]->kind == N_Name && !strcmp(iter->k.v[0]->s, "enumerate") &&
                      !lookup(L, intern("enumerate")) && (iter->k.n == 2 || iter->k.n == 3) && tuple_target && tuple_target->k.n == 2;
        Opnd seq = gen(L, is_enum ? iter->k.v[1] : iter);
        Opnd base = is_enum && iter->k.n == 3 ? gen(L, iter->k.v[2]) : o_int(0, TY_INT);
        if (seq.k != OP_TEMP) { Opnd t = temp(L, TY_DYN); put_copy(L, t, seq); seq = t; }
        Opnd i = temp(L, TY_INT);
        Instr *ci = emit(L, I_COPY);
        ci->d = i; ci->a = o_int(0, TY_INT); ci->ty = TY_INT;
        put_label(L, lcond);
        Opnd len = put_un(L, U_LEN, seq, TY_DYN);
        Opnd c = put_bin(L, B_LT, i, len, TY_DYN, TY_DYN);
        put_if(L, c, lelse, 0);
        Opnd item = temp(L, TY_DYN);
        Instr *ld = emit(L, I_LOAD);
        ld->d = item; ld->a = seq; ld->b = i; ld->ty = TY_DYN;
        if (is_enum) {
            LVal li = lval(L, tuple_target->k.v[0], 1);
            Opnd idx = base.k == OP_INT && base.i == 0 ? i : put_bin(L, B_ADD, i, base, TY_DYN, TY_DYN);
            lval_store(L, &li, idx);
            LVal lx = lval(L, tuple_target->k.v[1], 1);
            lval_store(L, &lx, item);
        } else if (tuple_target) {
            for (int k = 0; k < tuple_target->k.n; k++) {
                Opnd part = temp(L, TY_DYN);
                Instr *p = emit(L, I_LOAD);
                p->d = part; p->a = item; p->b = o_int(k, TY_INT); p->ty = TY_DYN;
                if (tuple_target->k.v[k]->kind != N_Name) { opaque(L, tuple_target->k.v[k], "a nested loop target", 0); continue; }
                LVal lx = lval(L, tuple_target->k.v[k], 1);
                lval_store(L, &lx, part);
            }
        } else {
            target = lval(L, &fake, 1);
            lval_store(L, &target, item);
        }
        loop_push(L, lend, lcont);
        stmt(L, body);
        loop_pop(L);
        put_label(L, lcont);
        Opnd nx = put_bin(L, B_ADD, i, o_int(1, TY_INT), TY_INT, TY_DYN);
        Instr *inc = emit(L, I_COPY);
        inc->d = i; inc->a = nx; inc->ty = TY_INT;
        put_goto(L, lcond);
    }
    put_label(L, lelse);
    if (els && els->kind != N_Empty) stmt(L, els);
    put_label(L, lend);
}

static void java_foreach(Lw *L, Node *n) {
    Opnd seq = gen(L, n->k.v[0]);
    if (seq.ty != TY_ARR && seq.ty != TY_UNKNOWN) { opaque(L, n, "for-each over a collection (only arrays are translated)", 0); return; }
    scope_push(L);
    Ty ty, el; int rank;
    type_of(L, n->type, &ty, &el, &rank);
    if (ty == TY_UNKNOWN) { ty = seq.rank > 1 ? TY_ARR : (Ty)seq.el; el = (Ty)seq.el; rank = seq.rank - 1; }
    Sym *s = declare(L, n, n->s, ty, el, rank, SK_VAR, n->type);
    Opnd var = sym_opnd(s);
    int lcond = label(L), lcont = label(L), lend = label(L);
    Opnd i = temp(L, TY_INT);
    put_copy(L, i, o_int(0, TY_INT));
    put_label(L, lcond);
    Opnd len = put_un(L, U_LEN, seq, TY_INT);
    Opnd c = put_bin(L, B_LT, i, len, TY_BOOL, TY_INT);
    put_if(L, c, lend, 0);
    Opnd item = temp(L, (Ty)seq.el);
    Instr *ld = emit(L, I_LOAD);
    ld->d = item; ld->a = seq; ld->b = i; ld->ty = seq.el;
    put_copy(L, var, assign_conv(L, n, item, ty, "the loop variable"));
    loop_push(L, lend, lcont);
    stmt(L, n->k.v[1]);
    loop_pop(L);
    put_label(L, lcont);
    put_copy(L, i, put_bin(L, B_ADD, i, o_int(1, TY_INT), TY_INT, TY_INT));
    put_goto(L, lcond);
    put_label(L, lend);
    scope_pop(L);
}

static void switch_stmt(Lw *L, Node *n) {
    Opnd subj = gen(L, n->k.v[0]);
    if (subj.k == OP_VAR) { Opnd t = temp(L, (Ty)subj.ty); put_copy(L, t, subj); subj = t; }
    int lend = label(L), ldef = -1;
    int ncase = n->k.n - 1;
    int *labs = xmalloc(sizeof(int) * (size_t)(ncase ? ncase : 1));
    for (int c = 0; c < ncase; c++) {
        Node *cs = n->k.v[c + 1];
        labs[c] = label(L);
        if (cs->s) { ldef = labs[c]; continue; }
        Opnd v = gen(L, cs->k.v[0]);
        if (v.ty == TY_STR && L->lang == LANG_JAVA) {
            put_param(L, subj); put_param(L, v);
            Opnd eq = put_call(L, "String.equals", 2, TY_BOOL, 1);
            put_if(L, eq, labs[c], 1);
        } else {
            Opnd eq = put_bin(L, B_EQ, subj, v, cmp_ty(L), arith_ty(L, (Ty)subj.ty, (Ty)v.ty));
            put_if(L, eq, labs[c], 1);
        }
    }
    put_goto(L, ldef >= 0 ? ldef : lend);
    vpush(L->brk, lend);
    vpush(L->cont, L->cont.n ? L->cont.v[L->cont.n - 1] : -1);
    scope_push(L);
    for (int c = 0; c < ncase; c++) {
        Node *cs = n->k.v[c + 1];
        put_label(L, labs[c]);
        for (int i = 1; i < cs->k.n; i++) stmt(L, cs->k.v[i]);   /* falls through to the next case */
    }
    scope_pop(L);
    loop_pop(L);
    put_label(L, lend);
    free(labs);
}

static void stmt(Lw *L, Node *n) {
    if (!n) return;
    int saved_line = L->line;
    L->line = n->line;
    switch (n->kind) {
    case N_Block:
        if (L->lang != LANG_PY) scope_push(L);
        for (int i = 0; i < n->k.n; i++) {
            int from = L->fn->code.n;
            stmt(L, n->k.v[i]);
            if (L->marks.n <= 2) trace_stmt(L, n->k.v[i], from);
        }
        if (L->lang != LANG_PY) scope_pop(L);
        break;
    case N_Assign: gen(L, n); break;                   /* Python assignment statements */
    case N_DeclGroup: for (int i = 0; i < n->k.n; i++) var_decl(L, n->k.v[i], 0); break;
    case N_VarDecl: var_decl(L, n, 0); break;
    case N_ExprStmt: {
        Node *e = n->k.v[0];
        if (e->kind == N_Call) call_expr(L, e, 0);
        else if (e->kind == N_StrLit && L->lang == LANG_PY) {}          /* docstring */
        else {
            gen(L, e);
            if (e->kind != N_Assign && e->kind != N_IncDec && L->lang != LANG_PY && e->kind != N_Unsupported)
                diag(L->ds, SEV_WARNING, "semantic", "Unused Value", n->line, n->col, NULL, "expression result is not used");
        }
        break;
    }
    case N_If: {
        int lelse = label(L), lend = label(L);
        branch_false(L, n->k.v[0], lelse);
        stmt(L, n->k.v[1]);
        if (n->k.n > 2) {
            put_goto(L, lend);
            put_label(L, lelse);
            stmt(L, n->k.v[2]);
            put_label(L, lend);
        } else put_label(L, lelse);
        break;
    }
    case N_While: {
        int lcond = label(L), lend = label(L), lelse = label(L);
        put_label(L, lcond);
        branch_false(L, n->k.v[0], lelse);
        loop_push(L, lend, lcond);
        stmt(L, n->k.v[1]);
        loop_pop(L);
        put_goto(L, lcond);
        put_label(L, lelse);
        if (n->k.n > 2) stmt(L, n->k.v[2]);
        put_label(L, lend);
        break;
    }
    case N_DoWhile: {
        int ltop = label(L), lcont = label(L), lend = label(L);
        put_label(L, ltop);
        loop_push(L, lend, lcont);
        stmt(L, n->k.v[0]);
        loop_pop(L);
        put_label(L, lcont);
        branch_true(L, n->k.v[1], ltop);
        put_label(L, lend);
        break;
    }
    case N_For: {
        scope_push(L);
        int lcond = label(L), lcont = label(L), lend = label(L);
        Node *init = n->k.v[0], *cond = n->k.v[1], *step = n->k.v[2], *body = n->k.v[3];
        if (init->kind == N_DeclGroup || init->kind == N_Block) stmt(L, init);
        else if (init->kind != N_Empty) gen(L, init);
        put_label(L, lcond);
        if (cond->kind != N_Empty) branch_false(L, cond, lend);
        loop_push(L, lend, lcont);
        stmt(L, body);
        loop_pop(L);
        put_label(L, lcont);
        if (step->kind == N_Block) for (int i = 0; i < step->k.n; i++) gen(L, step->k.v[i]->k.v[0]);
        else if (step->kind != N_Empty) gen(L, step);
        put_goto(L, lcond);
        put_label(L, lend);
        scope_pop(L);
        break;
    }
    case N_ForEach:
        if (L->lang == LANG_PY) py_for(L, n); else java_foreach(L, n);
        break;
    case N_Switch: switch_stmt(L, n); break;
    case N_Return: {
        Instr ret;
        memset(&ret, 0, sizeof(ret));
        Opnd v = o_none();
        if (n->k.n) {
            v = gen(L, n->k.v[0]);
            if (L->lang != LANG_PY && L->ret == TY_VOID)
                diag(L->ds, SEV_ERROR, "semantic", "Type Error", n->line, n->col, "Remove the value, or change the return type.", "a void function cannot return a value");
            else v = assign_conv(L, n, v, L->ret, "the return value");
        } else if (L->lang == LANG_PY) v = o_null();
        else if (L->ret != TY_VOID && L->ret != TY_UNKNOWN)
            diag(L->ds, L->lang == LANG_JAVA ? SEV_ERROR : SEV_WARNING, "semantic", "Missing Return Value", n->line, n->col, NULL,
                 "this function returns %s, but the return statement has no value", src_ty(L, L->ret));
        emit(L, I_RET)->a = v;
        break;
    }
    case N_Break: case N_Continue: {
        IntVec *st = n->kind == N_Break ? &L->brk : &L->cont;
        int lab = st->n ? st->v[st->n - 1] : -1;
        if (lab < 0) diag(L->ds, SEV_ERROR, "semantic", "Misplaced Statement", n->line, n->col, NULL,
                          "'%s' outside of a loop%s", n->kind == N_Break ? "break" : "continue", n->kind == N_Break ? " or switch" : "");
        else put_goto(L, lab);
        break;
    }
    case N_Pass: case N_Empty: case N_Import: case N_Directive: case N_Global: break;
    case N_Throw: {
        Opnd v = n->k.n ? gen(L, n->k.v[0]) : o_null();
        put_param(L, v);
        put_call(L, L->lang == LANG_PY ? "raise" : "throw", 1, TY_VOID, 0);
        break;
    }
    case N_Assert: {
        int ok = label(L);
        branch_true(L, n->k.v[0], ok);
        Opnd msg = n->k.n > 1 ? gen(L, n->k.v[1]) : o_str(intern(""));
        put_param(L, msg);
        put_call(L, "assert_fail", 1, TY_VOID, 0);
        put_label(L, ok);
        break;
    }
    case N_Try: {
        char what[96];
        snprintf(what, sizeof(what), "exception handling (%d handler block%s not translated)", n->k.n - 1, n->k.n == 2 ? "" : "s");
        opaque(L, n, what, 0);
        stmt(L, n->k.v[0]);
        break;
    }
    case N_Unsupported: opaque(L, n, n->s ? n->s : "an unsupported statement", 0); break;
    case N_Function: opaque(L, n, "a nested function definition", 0); break;
    case N_Class: opaque(L, n, "a nested class definition", 0); break;
    case N_StructDef: case N_EnumDef: break;
    default: {
        char what[64]; snprintf(what, sizeof(what), "a %s statement", node_kind_name(n->kind));
        opaque(L, n, what, 0);
    }
    }
    L->line = saved_line;
}

/* ---------------------------------------------------------------------
 * Functions and programs
 * ------------------------------------------------------------------- */

static Func *new_func(Lw *L, const char *name, int line) {
    Func f;
    memset(&f, 0, sizeof(f));
    f.name = intern(name);
    f.line = line;
    vpush(L->P->f, f);
    return &L->P->f.v[L->P->f.n - 1];
}

/* Unsupported file- or class-level items go into a separate, never-executed
 * function so they are shown and counted without changing what runs. */
static void aux_opaque(Lw *L, const char *fname, Node *n, const char *what) {
    Func *f = ir_find(L->P, fname);
    if (!f) f = new_func(L, fname, n->line);
    L->fn = f;
    L->line = n->line;
    opaque(L, n, what, 0);
}

static void end_func(Lw *L) {
    Instr *last = L->fn->code.n ? &L->fn->code.v[L->fn->code.n - 1] : NULL;
    if (!last || last->op != I_RET) {
        Opnd v = L->lang == LANG_PY ? o_null() : o_none();
        if (L->lang == LANG_C && !strcmp(L->fn->name, "main")) v = o_int(0, TY_INT);   /* main returns 0 */
        emit(L, I_RET)->a = v;
    }
    /* unused locals are worth a note */
    for (int i = 0; i < L->info->syms.n; i++) {
        SymInfo *s = &L->info->syms.v[i];
        if (s->scope == L->scope && s->uses == 0 && !strcmp(s->kind, "variable") && L->lang != LANG_PY)
            diag(L->ds, SEV_NOTE, "semantic", "Unused Variable", s->line, s->col, NULL, "variable '%s' is declared but never read", s->name);
    }
}

/* Python: the names a function assigns are its locals */
static void py_assigned(Node *n, NameVec *out, NameVec *glob) {
    if (!n) return;
    switch (n->kind) {
    case N_Function: case N_Class: vpush(*out, n->s); return;
    case N_Lambda: case N_Comprehension: return;
    case N_Global: vpush(*glob, n->s); return;
    case N_Assign:
        for (int i = 0; i < n->k.n - 1; i++) {
            Node *t = n->k.v[i];
            if (t->kind == N_Name) vpush(*out, t->s);
            else if (t->kind == N_Tuple || t->kind == N_ArrayInit)
                for (int j = 0; j < t->k.n; j++) if (t->k.v[j]->kind == N_Name) vpush(*out, t->k.v[j]->s);
        }
        break;
    case N_ForEach:
        if (n->s) vpush(*out, n->s);
        if (n->ival && n->k.n > 3) for (int j = 0; j < n->k.v[3]->k.n; j++) if (n->k.v[3]->k.v[j]->kind == N_Name) vpush(*out, n->k.v[3]->k.v[j]->s);
        break;
    case N_Import: {
        /* import a.b as c / from x import y */
        const char *s = n->s;
        if (!strncmp(s, "import ", 7)) {
            const char *as = strstr(s, " as ");
            const char *nm = as ? as + 4 : s + 7;
            vpush(*out, intern_n(nm, strcspn(nm, " .,")));
        }
        break;
    }
    default: break;
    }
    for (int i = 0; i < n->k.n; i++) py_assigned(n->k.v[i], out, glob);
}

static FnSig *add_sig(Lw *L, const char *name, const char *ir, Ty ret) {
    FnSig f;
    memset(&f, 0, sizeof(f));
    f.name = intern(name); f.ir = intern(ir); f.ret = ret;
    vpush(L->fns, f);
    return &L->fns.v[L->fns.n - 1];
}

static void sig_params(Lw *L, FnSig *f, Node *params, int self) {
    if (self) { vpush(f->ptys, TY_OBJ); vpush(f->pnames, intern("this")); vpush(f->defaults, NULL); f->nparams++; }
    for (int i = 0; i < params->k.n; i++) {
        Node *p = params->k.v[i];
        if (p->s && !strcmp(p->s, "...")) { f->variadic = 1; continue; }
        if (p->type && (!strcmp(p->type, "*args") || !strcmp(p->type, "**kwargs"))) { f->variadic = 1; continue; }
        Ty ty, el; int rank;
        type_of(L, p->type, &ty, &el, &rank);
        vpush(f->ptys, (unsigned char)ty);
        vpush(f->pnames, p->s);
        vpush(f->defaults, p->k.n ? p->k.v[0] : NULL);
        f->nparams++;
    }
}

static void lower_function(Lw *L, Node *fn, const char *irname, const char *cls, int self_param) {
    if (fn->flags & NF_PROTOTYPE) return;
    L->fn = new_func(L, irname, fn->line);
    L->scope = L->fn->name;
    L->cls = cls;
    Ty ret, el; int rank;
    type_of(L, fn->type, &ret, &el, &rank);
    L->ret = L->lang == LANG_PY ? TY_DYN : ret;
    L->fn->ret = L->ret;
    L->line = fn->line;
    scope_push(L);
    L->locals.n = 0; L->gdecl.n = 0;
    L->py_module = 0;
    Node *params = fn->k.v[0];
    if (self_param && L->lang == LANG_JAVA) {
        Sym *s = declare(L, fn, intern("this"), TY_OBJ, TY_OBJ, 0, SK_PARAM, cls);
        vpush(L->fn->params, s->ir); vpush(L->fn->ptys, TY_OBJ);
    }
    if (L->lang == LANG_PY) {
        for (int i = 0; i < params->k.n; i++) vpush(L->locals, params->k.v[i]->s);
        py_assigned(fn->k.v[1], &L->locals, &L->gdecl);
    }
    for (int i = 0; i < params->k.n; i++) {
        Node *p = params->k.v[i];
        if (!p->s || !strcmp(p->s, "...")) continue;
        if (p->type && (!strcmp(p->type, "*args") || !strcmp(p->type, "**kwargs"))) { opaque(L, p, "variadic *args / **kwargs parameters", 0); continue; }
        Ty ty; int r;
        type_of(L, p->type, &ty, &el, &r);
        Sym *s = declare(L, p, p->s, ty, el, r, SK_PARAM, p->type ? p->type : "dynamic");
        vpush(L->fn->params, s->ir);
        vpush(L->fn->ptys, (unsigned char)ty);
    }
    vfree(L->brk); vfree(L->cont);
    stmt(L, fn->k.v[1]);
    end_func(L);
    scope_pop(L);
    if (L->lang != LANG_PY && L->ret != TY_VOID && L->ret != TY_UNKNOWN && strcmp(L->fn->name, "main")) {
        /* a non-void function whose last statement is not a return can fall off its end */
        Node *body = fn->k.v[1];
        Node *last = body->k.n ? body->k.v[body->k.n - 1] : NULL;
        if (!last || (last->kind != N_Return && last->kind != N_Throw && last->kind != N_While && last->kind != N_For && last->kind != N_If && last->kind != N_Switch))
            diag(L->ds, L->lang == LANG_JAVA ? SEV_ERROR : SEV_WARNING, "semantic", "Missing Return", fn->line, fn->col, "End the function with a return statement.",
                 "%s() can reach its end without returning a %s", fn->s, src_ty(L, L->ret));
    }
}

static void add_global(Lw *L, Node *at, const char *name, const char *ir, Ty ty, Ty el, int rank, int kind, const char *type_text) {
    Sym s = {0};
    s.name = intern(name); s.ir = intern(ir); s.ty = (unsigned char)ty; s.el = (unsigned char)el; s.rank = (unsigned char)rank;
    s.kind = (unsigned char)kind; s.global = 1; s.line = at->line; s.col = at->col;
    const char *saved = L->scope; L->scope = "global";
    s.info = record_sym(L, s.name, kind, type_text, at->line, at->col);
    L->scope = saved;
    vpush(L->globals, s);
    if (kind == SK_GLOBAL) { GlobalVar g = { s.ir, ty, at->line }; vpush(L->P->globals, g); }
}

static void define_constant(Lw *L, Node *at, const char *name, Opnd v, int kind) {
    add_global(L, at, name, name, (Ty)v.ty, TY_UNKNOWN, 0, kind, ty_name((Ty)v.ty));
    Sym *s = &L->globals.v[L->globals.n - 1];
    s->has_cval = 1;
    s->cval_op = v;
}

/* #define NAME <number> becomes a constant; other macros are remembered as macros */
static void c_define(Lw *L, Node *d) {
    const char *s = d->s + 1;
    while (*s == ' ' || *s == '\t') s++;
    if (strncmp(s, "define", 6)) return;
    s += 6;
    while (*s == ' ' || *s == '\t') s++;
    size_t nl = strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789");
    if (!nl) return;
    const char *name = intern_n(s, nl);
    const char *rest = s + nl;
    if (*rest == '(') { add_global(L, d, name, name, TY_UNKNOWN, TY_UNKNOWN, 0, SK_MACRO, "function-like macro"); return; }
    while (*rest == ' ' || *rest == '\t') rest++;
    char val[64];
    size_t vl = strcspn(rest, "\n/");
    while (vl && (rest[vl - 1] == ' ' || rest[vl - 1] == '\t' || rest[vl - 1] == '\r')) vl--;
    if (vl == 0 || vl >= sizeof(val)) { add_global(L, d, name, name, TY_UNKNOWN, TY_UNKNOWN, 0, SK_MACRO, "macro"); return; }
    memcpy(val, rest, vl); val[vl] = 0;
    char *v = val;
    int neg = 0;
    if (*v == '(' && v[vl - 1] == ')') { v[vl - 1] = 0; v++; }
    if (*v == '-') { neg = 1; v++; }
    int ok;
    if (is_float_literal(v, LANG_C)) {
        char *end; double f = strtod(v, &end);
        if (*end == 0 || ((*end == 'f' || *end == 'F') && end[1] == 0)) { define_constant(L, d, name, o_flt(neg ? -f : f, TY_DOUBLE), SK_CONST); return; }
    } else {
        long long x = parse_int_literal(v, &ok);
        if (ok && v[0] >= '0' && v[0] <= '9') { define_constant(L, d, name, o_int(neg ? -x : x, x > INT_MAX ? TY_LONG : TY_INT), SK_CONST); return; }
    }
    add_global(L, d, name, name, TY_UNKNOWN, TY_UNKNOWN, 0, SK_MACRO, "macro");
}

static long long const_eval(Lw *L, Node *e, int *ok) {
    if (e->kind == N_IntLit || e->kind == N_CharLit) return e->ival;
    if (e->kind == N_Unary && !strcmp(e->s, "-")) return -const_eval(L, e->k.v[0], ok);
    if (e->kind == N_Name) { Sym *s = lookup(L, e->s); if (s && s->has_cval && s->cval_op.k == OP_INT) return s->cval_op.i; }
    if (e->kind == N_Binary && e->k.n == 2) {
        long long a = const_eval(L, e->k.v[0], ok), b = const_eval(L, e->k.v[1], ok);
        switch (e->s[0]) { case '+': return a + b; case '-': return a - b; case '*': return a * b;
                           case '/': return b ? a / b : (*ok = 0); case '<': return a << b; }
    }
    *ok = 0;
    return 0;
}

static void lower_c(Lw *L, Node *prog) {
    /* pass 1: everything at file scope */
    VEC(Node *) items = {0};
    for (int i = 0; i < prog->k.n; i++) {
        Node *n = prog->k.v[i];
        if (n->kind == N_Block) { for (int j = 0; j < n->k.n; j++) vpush(items, n->k.v[j]); }
        else vpush(items, n);
    }
    for (int i = 0; i < items.n; i++) {
        Node *n = items.v[i];
        if (n->kind == N_Directive) c_define(L, n);
        else if (n->kind == N_EnumDef) {
            long long next = 0;
            for (int j = 0; j < n->k.n; j++) {
                Node *en = n->k.v[j];
                if (en->k.n) { int ok = 1; long long v = const_eval(L, en->k.v[0], &ok); if (ok) next = v; }
                define_constant(L, en, en->s, o_int(next, TY_INT), SK_CONST);
                next++;
            }
        } else if (n->kind == N_Function) {
            Ty ret, el; int rank;
            type_of(L, n->type, &ret, &el, &rank);
            FnSig *f = find_fn(L, n->s, -1);
            if (!f) {
                f = add_sig(L, n->s, n->s, ret);
                sig_params(L, f, n->k.v[0], 0);
                add_global(L, n, n->s, n->s, ret, TY_UNKNOWN, 0, SK_FUNC, n->type);
            }
        } else if (n->kind == N_DeclGroup) {
            for (int j = 0; j < n->k.n; j++) {
                Node *d = n->k.v[j];
                if (d->kind != N_VarDecl || !d->s) continue;
                Ty ty, el; int rank;
                type_of(L, d->type, &ty, &el, &rank);
                if (L->lang == LANG_C && ty == TY_STR && d->ival > 0 && d->k.v[0]->kind != N_StrLit) { ty = TY_ARR; el = TY_CHAR; rank = (int)d->ival; }
                if (lookup(L, d->s)) diag(L->ds, SEV_ERROR, "semantic", "Duplicate Declaration", d->line, d->col, NULL, "'%s' is already declared at file scope", d->s);
                add_global(L, d, d->s, d->s, ty, el, rank, SK_GLOBAL, d->type);
            }
        }
    }
    /* global initializers run before main */
    int has_globals = 0;
    for (int i = 0; i < items.n; i++) if (items.v[i]->kind == N_DeclGroup) has_globals = 1;
    if (has_globals) {
        L->fn = new_func(L, "<globals>", 1);
        L->scope = "global";
        L->ret = TY_VOID;
        for (int i = 0; i < items.n; i++)
            if (items.v[i]->kind == N_DeclGroup)
                for (int j = 0; j < items.v[i]->k.n; j++) { L->line = items.v[i]->k.v[j]->line; var_decl(L, items.v[i]->k.v[j], 1); }
        end_func(L);
        vpush(L->P->entry, L->fn->name);
    }
    int has_main = 0;
    for (int i = 0; i < items.n; i++) {
        Node *n = items.v[i];
        if (n->kind == N_Function && !(n->flags & NF_PROTOTYPE)) {
            lower_function(L, n, n->s, NULL, 0);
            if (!strcmp(n->s, "main")) has_main = 1;
        } else if (n->kind == N_Unsupported) aux_opaque(L, "<declarations>", n, n->s);
    }
    if (has_main) vpush(L->P->entry, intern("main"));
    vfree(items);
}

static void lower_java(Lw *L, Node *prog) {
    /* pass 1: classes, static fields, method signatures */
    VEC(Node *) classes = {0};
    for (int i = 0; i < prog->k.n; i++) {
        Node *c = prog->k.v[i];
        if (c->kind == N_Class) vpush(classes, c);
        else if (c->kind == N_Unsupported) { /* interfaces / enums: reported when used */ }
    }
    for (int i = 0; i < classes.n; i++) for (int j = 0; j < classes.v[i]->k.n; j++)
        if (classes.v[i]->k.v[j]->kind == N_Class) vpush(classes, classes.v[i]->k.v[j]);
    for (int i = 0; i < classes.n; i++) {
        Node *c = classes.v[i];
        add_global(L, c, c->s, c->s, TY_OBJ, TY_UNKNOWN, 0, SK_CLASS, "class");
        vpush(L->classes, c->s);
    }
    for (int i = 0; i < classes.n; i++) {
        Node *c = classes.v[i];
        for (int j = 0; j < c->k.n; j++) {
            Node *m = c->k.v[j];
            char q[256];
            if (m->kind == N_Function && m->s[0] != '<') {
                snprintf(q, sizeof(q), "%s.%s", c->s, m->s);
                int overloads = 0;
                for (int k = 0; k < L->fns.n; k++) if (!strcmp(L->fns.v[k].name, q)) overloads++;
                char ir[280];
                if (overloads) snprintf(ir, sizeof(ir), "%s#%d", q, overloads + 1); else snprintf(ir, sizeof(ir), "%s", q);
                Ty ret, el; int rank;
                type_of(L, m->type, &ret, &el, &rank);
                FnSig *f = add_sig(L, q, ir, ret);
                f->is_static = (m->flags & NF_STATIC) != 0;
                sig_params(L, f, m->k.v[0], !f->is_static);
            } else if (m->kind == N_DeclGroup) {
                for (int k = 0; k < m->k.n; k++) {
                    Node *d = m->k.v[k];
                    if (d->kind != N_VarDecl || !d->s) continue;
                    Ty ty, el; int rank;
                    type_of(L, d->type, &ty, &el, &rank);
                    snprintf(q, sizeof(q), "%s.%s", c->s, d->s);
                    if (d->flags & NF_STATIC) {
                        /* static final with a literal value: a compile-time constant */
                        Node *init = d->k.v[0];
                        if ((d->flags & NF_CONST) && (init->kind == N_IntLit || init->kind == N_FloatLit || init->kind == N_StrLit || init->kind == N_BoolLit || init->kind == N_CharLit) && ty != TY_UNKNOWN) {
                            Opnd v = init->kind == N_IntLit ? o_int(init->ival, ty == TY_LONG ? TY_LONG : TY_INT) :
                                     init->kind == N_FloatLit ? o_flt(init->fval, ty == TY_FLOAT ? TY_FLOAT : TY_DOUBLE) :
                                     init->kind == N_StrLit ? o_str(init->s) : init->kind == N_CharLit ? o_int(init->ival, TY_CHAR) : o_bool((int)init->ival);
                            if (ty == TY_DOUBLE && v.k == OP_INT) v = o_flt((double)v.i, TY_DOUBLE);
                            define_constant(L, d, q, v, SK_CONST);
                            L->globals.v[L->globals.n - 1].name = intern(q);
                        } else add_global(L, d, q, q, ty, el, rank, SK_GLOBAL, d->type);
                    } else {
                        add_global(L, d, d->s, d->s, ty, el, rank, SK_FIELD, d->type);
                    }
                }
            }
        }
    }
    /* static initialization, then methods */
    const char *main_fn = NULL;
    for (int i = 0; i < classes.n; i++) {
        Node *c = classes.v[i];
        int needs_clinit = 0;
        for (int j = 0; j < c->k.n; j++) {
            Node *m = c->k.v[j];
            if ((m->kind == N_DeclGroup && (m->k.n && (m->k.v[0]->flags & NF_STATIC))) ||
                (m->kind == N_Function && !strcmp(m->s, "<clinit-block>"))) needs_clinit = 1;
        }
        if (needs_clinit) {
            char q[256]; snprintf(q, sizeof(q), "%s.<clinit>", c->s);
            L->fn = new_func(L, q, c->line);
            L->scope = L->fn->name; L->cls = c->s; L->ret = TY_VOID;
            scope_push(L);
            for (int j = 0; j < c->k.n; j++) {
                Node *m = c->k.v[j];
                if (m->kind == N_DeclGroup && m->k.n && (m->k.v[0]->flags & NF_STATIC)) {
                    for (int k = 0; k < m->k.n; k++) {
                        Node *d = m->k.v[k];
                        if (d->kind != N_VarDecl || !d->s) continue;
                        char fq[256]; snprintf(fq, sizeof(fq), "%s.%s", c->s, d->s);
                        Sym *s = lookup(L, intern(fq));
                        if (!s || s->has_cval) continue;
                        L->line = d->line;
                        const char *orig = d->s;
                        d->s = intern(fq);
                        var_decl(L, d, 1);
                        d->s = orig;
                    }
                } else if (m->kind == N_Function && !strcmp(m->s, "<clinit-block>")) stmt(L, m->k.v[1]);
            }
            scope_pop(L);
            end_func(L);
            vpush(L->P->entry, L->fn->name);
        }
    }
    for (int i = 0; i < classes.n; i++) {
        Node *c = classes.v[i];
        for (int j = 0; j < c->k.n; j++) {
            Node *m = c->k.v[j];
            if (m->kind == N_Function) {
                if (!strcmp(m->s, "<clinit-block>")) continue;
                if (!strcmp(m->s, "<init-block>")) { aux_opaque(L, "<declarations>", m, "an instance initializer block"); continue; }
                char q[256]; snprintf(q, sizeof(q), "%s.%s", c->s, m->s);
                const char *ir = q;
                FnSig *f = NULL;
                for (int k = 0; k < L->fns.n; k++) {
                    FnSig *g = &L->fns.v[k];
                    if (!strcmp(g->name, q) && g->nparams == m->k.v[0]->k.n + !g->is_static && !ir_find(L->P, g->ir)) { f = g; break; }
                }
                if (f) ir = f->ir;
                lower_function(L, m, ir, c->s, !(m->flags & NF_STATIC));
                if (!strcmp(m->s, "main") && (m->flags & NF_STATIC) && !main_fn) main_fn = intern(ir);
            } else if (m->kind == N_DeclGroup && m->k.n && !(m->k.v[0]->flags & NF_STATIC)) {
                for (int k = 0; k < m->k.n; k++)
                    if (m->k.v[k]->kind == N_VarDecl && m->k.v[k]->k.n && m->k.v[k]->k.v[0]->kind != N_Empty)
                        aux_opaque(L, "<declarations>", m->k.v[k], "an instance field initializer");
            } else if (m->kind == N_Unsupported) {
                aux_opaque(L, "<declarations>", m, m->s);
            }
        }
    }
    for (int i = 0; i < prog->k.n; i++)
        if (prog->k.v[i]->kind == N_Unsupported) aux_opaque(L, "<declarations>", prog->k.v[i], prog->k.v[i]->s);
    if (main_fn) vpush(L->P->entry, main_fn);
    vfree(classes);
}

static void lower_python(Lw *L, Node *prog) {
    /* pass 1: module-level names, functions, classes, imports */
    NameVec names = {0}, glob = {0};
    py_assigned(prog, &names, &glob);
    for (int i = 0; i < prog->k.n; i++) {
        Node *n = prog->k.v[i];
        if (n->kind == N_Function) {
            FnSig *f = add_sig(L, n->s, n->s, TY_DYN);
            sig_params(L, f, n->k.v[0], 0);
            add_global(L, n, n->s, n->s, TY_DYN, TY_UNKNOWN, 0, SK_FUNC, "function");
        } else if (n->kind == N_Class) {
            add_global(L, n, n->s, n->s, TY_DYN, TY_UNKNOWN, 0, SK_CLASS, "class");
            for (int j = 0; j < n->k.n; j++) {
                Node *m = n->k.v[j];
                if (m->kind != N_Function) continue;
                char q[256]; snprintf(q, sizeof(q), "%s.%s", n->s, m->s);
                FnSig *f = add_sig(L, q, q, TY_DYN);
                sig_params(L, f, m->k.v[0], 0);
            }
        } else if (n->kind == N_Import) {
            const char *s = n->s;
            if (!strncmp(s, "import ", 7)) {
                const char *as = strstr(s, " as ");
                const char *nm = as ? as + 4 : s + 7;
                add_global(L, n, intern_n(nm, strcspn(nm, " .,")), intern_n(nm, strcspn(nm, " .,")), TY_DYN, TY_UNKNOWN, 0, SK_MODULE, "module");
            } else {
                /* from m import a, b: the names come from outside this file */
                const char *imp = strstr(s, " import ");
                if (imp) {
                    imp += 8;
                    while (*imp) {
                        while (*imp == ' ' || *imp == ',' || *imp == '(') imp++;
                        size_t k = strcspn(imp, " ,)");
                        if (!k) break;
                        const char *nm = intern_n(imp, k);
                        imp += k;
                        if (!strncmp(imp, " as ", 4)) { imp += 4; k = strcspn(imp, " ,)"); nm = intern_n(imp, k); imp += k; }
                        add_global(L, n, nm, nm, TY_DYN, TY_UNKNOWN, 0, SK_MODULE, "imported name");
                    }
                }
            }
        }
    }
    for (int i = 0; i < names.n; i++) {
        if (lookup(L, names.v[i])) continue;
        Node at = {0};
        add_global(L, &at, names.v[i], names.v[i], TY_DYN, TY_UNKNOWN, 0, SK_GLOBAL, "dynamic");
    }
    /* module code */
    L->fn = new_func(L, "<module>", 1);
    L->scope = "module";
    L->py_module = 1;
    L->ret = TY_DYN;
    vpush(L->P->entry, L->fn->name);
    for (int i = 0; i < prog->k.n; i++) {
        Node *n = prog->k.v[i];
        if (n->kind == N_Function || n->kind == N_Import) continue;
        int from = L->fn->code.n;
        if (n->kind == N_Class) {
            for (int j = 0; j < n->k.n; j++) {
                Node *m = n->k.v[j];
                if (m->kind == N_Function || (m->kind == N_ExprStmt && m->k.v[0]->kind == N_StrLit) || m->kind == N_Pass) continue;
                L->line = m->line;
                opaque(L, m, "a class-level statement", 0);
            }
            continue;
        }
        stmt(L, n);
        trace_stmt(L, n, from);
    }
    end_func(L);
    for (int i = 0; i < prog->k.n; i++) {
        Node *n = prog->k.v[i];
        if (n->kind == N_Function) lower_function(L, n, n->s, NULL, 0);
        if (n->kind == N_Class)
            for (int j = 0; j < n->k.n; j++) if (n->k.v[j]->kind == N_Function) {
                char q[256]; snprintf(q, sizeof(q), "%s.%s", n->s, n->k.v[j]->s);
                lower_function(L, n->k.v[j], q, n->s, 0);
            }
    }
    vfree(names); vfree(glob);
}

int lower_program(Node *ast, Lang lang, IRProg *out, Diags *ds, LowerInfo *info) {
    memset(out, 0, sizeof(*out));
    out->lang = lang;
    Lw L;
    memset(&L, 0, sizeof(L));
    L.lang = lang; L.P = out; L.ds = ds; L.info = info;
    L.scope = "global";
    if (lang == LANG_C) lower_c(&L, ast);
    else if (lang == LANG_JAVA) lower_java(&L, ast);
    else lower_python(&L, ast);
    info->complete = info->opaque == 0;
    vfree(L.syms); vfree(L.marks); vfree(L.globals); vfree(L.brk); vfree(L.cont);
    vfree(L.locals); vfree(L.gdecl); vfree(L.classes);
    for (int i = 0; i < L.fns.n; i++) { vfree(L.fns.v[i].ptys); vfree(L.fns.v[i].pnames); vfree(L.fns.v[i].defaults); }
    vfree(L.fns);
    return 0;
}
