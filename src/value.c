#include "value.h"
#include <math.h>
#include <limits.h>

Val v_int(long long i) { Val v = {0}; v.k = V_INT; v.i = i; return v; }
Val v_bool(int b) { Val v = {0}; v.k = V_BOOL; v.i = b != 0; return v; }
Val v_dbl(double d) { Val v = {0}; v.k = V_DBL; v.d = d; return v; }
Val v_none(void) { Val v = {0}; v.k = V_NONE; return v; }

static Obj *obj_new(Heap *h, size_t extra) {
    Obj *o = xcalloc(1, sizeof(Obj));
    o->next = h->all;
    h->all = o;
    h->bytes += sizeof(Obj) + extra;
    return o;
}

Val v_str(Heap *h, const char *s, size_t n) {
    Obj *o = obj_new(h, n);
    o->s = xstrndup(s, n);
    o->slen = n;
    Val v = {0}; v.k = V_STR; v.o = o;
    return v;
}

Val v_seq(Heap *h, SeqKind kind, int n, Ty elem) {
    Obj *o = obj_new(h, sizeof(Val) * (size_t)(n > 0 ? n : 0));
    o->seq = (unsigned char)kind;
    o->elem = (unsigned char)elem;
    o->n = n > 0 ? n : 0;
    o->cap = o->n > 4 ? o->n : 4;
    o->items = xcalloc((size_t)o->cap, sizeof(Val));
    Val zero = v_none();
    if (kind == SEQ_ARRAY) {
        if (elem == TY_DOUBLE || elem == TY_FLOAT) { zero = v_dbl(0); zero.f32 = elem == TY_FLOAT; }
        else if (elem == TY_BOOL) zero = v_bool(0);
        else if (elem == TY_CHAR) { zero = v_int(0); zero.k = V_CHAR; }
        else if (ty_is_int(elem)) zero = v_int(0);
    }
    for (int i = 0; i < o->n; i++) o->items[i] = zero;
    Val v = {0}; v.k = V_SEQ; v.o = o;
    return v;
}

void heap_free(Heap *h) {
    Obj *o = h->all;
    while (o) { Obj *n = o->next; free(o->s); free(o->items); free(o); o = n; }
    h->all = NULL;
    h->bytes = 0;
}

int v_truthy(const Val *v) {
    switch (v->k) {
    case V_NONE: case V_UNDEF: return 0;
    case V_DBL: return v->d != 0;
    case V_STR: return v->o->slen != 0;
    case V_SEQ: return v->o->n != 0;
    default: return v->i != 0;
    }
}
int v_is_num(const Val *v) { return v->k == V_INT || v->k == V_BOOL || v->k == V_CHAR || v->k == V_DBL; }
double v_as_dbl(const Val *v) { return v->k == V_DBL ? v->d : (double)v->i; }

/* ---- number formatting ---- */

/* shortest decimal digits that read back as d (as a float when f32) */
static void shortest(double d, int f32, char *digits, int *exp10) {
    char tmp[48];
    for (int p = 1; p <= 17; p++) {
        snprintf(tmp, sizeof(tmp), "%.*e", p - 1, d);
        if (f32 ? (strtof(tmp, NULL) == (float)d) : (strtod(tmp, NULL) == d)) break;
    }
    /* tmp = "[-]d.ddde[+-]xx" */
    char *s = tmp;
    if (*s == '-') s++;
    int k = 0;
    for (; *s && *s != 'e'; s++) if (*s != '.') digits[k++] = *s;
    while (k > 1 && digits[k - 1] == '0') k--;
    digits[k] = 0;
    *exp10 = atoi(s + 1);
}

void fmt_double(Buf *b, double d, Lang lang, int f32) {
    if (isnan(d)) { bputs(b, lang == LANG_PY ? "nan" : "NaN"); return; }
    if (isinf(d)) { bputs(b, lang == LANG_PY ? (d > 0 ? "inf" : "-inf") : (d > 0 ? "Infinity" : "-Infinity")); return; }
    if (lang == LANG_C) { bprintf(b, "%g", d); return; }
    if (d == 0) { bputs(b, signbit(d) ? "-0.0" : "0.0"); return; }
    char dg[40];
    int e;
    shortest(d, f32, dg, &e);
    if (d < 0) bputc(b, '-');
    int nd = (int)strlen(dg);
    int sci = lang == LANG_PY ? (e < -4 || e >= 16) : (e < -3 || e >= 7);
    if (!sci) {
        if (e < 0) { bputs(b, "0."); for (int i = -1; i > e; i--) bputc(b, '0'); bputs(b, dg); }
        else {
            for (int i = 0; i <= e; i++) bputc(b, i < nd ? dg[i] : '0');
            bputc(b, '.');
            if (nd > e + 1) bputs(b, dg + e + 1); else bputc(b, '0');
        }
        return;
    }
    bputc(b, dg[0]);
    if (lang == LANG_PY) {
        if (nd > 1) { bputc(b, '.'); bputs(b, dg + 1); }
        bprintf(b, "e%c%02d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    } else {
        bputc(b, '.');
        bputs(b, nd > 1 ? dg + 1 : "0");
        bprintf(b, "E%d", e);
    }
}

void v_tostr(Buf *b, const Val *v, Lang lang, int repr) {
    switch (v->k) {
    case V_UNDEF: bputs(b, "<undefined>"); break;
    case V_NONE: bputs(b, lang == LANG_PY ? "None" : "null"); break;
    case V_BOOL:
        if (lang == LANG_PY) bputs(b, v->i ? "True" : "False");
        else if (lang == LANG_JAVA) bputs(b, v->i ? "true" : "false");
        else bprintf(b, "%lld", v->i);
        break;
    case V_CHAR:
        if (lang == LANG_C) { bprintf(b, "%lld", v->i); break; }
        if (v->i < 0x80) bputc(b, (char)v->i);
        else if (v->i < 0x800) { bputc(b, (char)(0xC0 | (v->i >> 6))); bputc(b, (char)(0x80 | (v->i & 63))); }
        else { bputc(b, (char)(0xE0 | (v->i >> 12))); bputc(b, (char)(0x80 | ((v->i >> 6) & 63))); bputc(b, (char)(0x80 | (v->i & 63))); }
        break;
    case V_INT: bprintf(b, "%lld", v->i); break;
    case V_DBL: fmt_double(b, v->d, lang, v->f32); break;
    case V_STR:
        if (repr && lang == LANG_PY) {
            int dq = memchr(v->o->s, '\'', v->o->slen) && !memchr(v->o->s, '"', v->o->slen);
            bputc(b, dq ? '"' : '\'');
            for (size_t i = 0; i < v->o->slen; i++) {
                char c = v->o->s[i];
                if (c == '\\') bputs(b, "\\\\");
                else if (c == '\n') bputs(b, "\\n");
                else if (c == '\t') bputs(b, "\\t");
                else if (c == '\r') bputs(b, "\\r");
                else if (c == (dq ? '"' : '\'')) { bputc(b, '\\'); bputc(b, c); }
                else bputc(b, c);
            }
            bputc(b, dq ? '"' : '\'');
        } else bput(b, v->o->s, v->o->slen);
        break;
    case V_SEQ: {
        Obj *o = v->o;
        if (lang == LANG_JAVA && o->seq == SEQ_ARRAY) { bprintf(b, "[%s@%x", ty_name((Ty)o->elem), (unsigned)((size_t)o >> 4) & 0xffffff); break; }
        bputc(b, o->seq == SEQ_TUPLE ? '(' : '[');
        for (int i = 0; i < o->n; i++) {
            if (i) bputs(b, ", ");
            v_tostr(b, &o->items[i], lang, 1);
        }
        if (o->seq == SEQ_TUPLE && o->n == 1) bputc(b, ',');
        bputc(b, o->seq == SEQ_TUPLE ? ')' : ']');
        break;
    }
    }
}

/* ---- integer arithmetic with explicit width ---- */

static int width_of(Ty ty) { return ty == TY_LONG ? 64 : 32; }

/* Fits v into the type's width. Returns 1 if it already fitted. */
static int fit(long long v, Ty ty, long long *out) {
    if (ty == TY_UINT) { *out = (long long)(unsigned)v; return v >= 0 && v <= UINT_MAX; }
    if (width_of(ty) == 32) { *out = (int)(unsigned)v; return v >= INT_MIN && v <= INT_MAX; }
    *out = v;
    return 1;
}

static SemResult int_op(Lang lang, Ty ty, BinOp op, long long x, long long y, Val *out, char *err, size_t el) {
    long long r = 0;
    int ovf = 0;
    int py = lang == LANG_PY;
    int wide = py || width_of(ty) == 64;
    switch (op) {
    case B_ADD: ovf = __builtin_add_overflow(x, y, &r); break;
    case B_SUB: ovf = __builtin_sub_overflow(x, y, &r); break;
    case B_MUL: ovf = __builtin_mul_overflow(x, y, &r); break;
    case B_DIV: case B_MOD: case B_FLOORDIV:
        if (y == 0) {
            if (lang == LANG_JAVA) snprintf(err, el, "java.lang.ArithmeticException: / by zero");
            else if (py) snprintf(err, el, "ZeroDivisionError: integer %s by zero", op == B_MOD ? "modulo" : "division");
            else { snprintf(err, el, "division by zero (undefined behaviour in C)"); return SEM_UB; }
            return SEM_TRAP;
        }
        if (x == LLONG_MIN && y == -1) { ovf = 1; r = LLONG_MIN; if (op == B_MOD) { r = 0; ovf = 0; } break; }
        if (op == B_DIV && !py) r = x / y;
        else if (op == B_MOD && !py) r = x % y;
        else {                                   /* Python: floor division, result sign follows the divisor */
            long long q = x / y, m = x % y;
            if (m != 0 && ((m < 0) != (y < 0))) { q--; m += y; }
            r = op == B_MOD ? m : q;
        }
        break;
    case B_POW: {
        if (y < 0) return SEM_UNSUP;             /* handled as float by the caller */
        r = 1;
        long long base = x;
        for (long long e = y; e > 0; e >>= 1) {
            if (e & 1) { if (__builtin_mul_overflow(r, base, &r)) { ovf = 1; break; } }
            if (e > 1 && __builtin_mul_overflow(base, base, &base)) { ovf = 1; break; }
        }
        break;
    }
    case B_AND: r = x & y; break;
    case B_OR:  r = x | y; break;
    case B_XOR: r = x ^ y; break;
    case B_SHL: case B_SHR: case B_USHR: {
        int w = wide ? 64 : 32;
        if (lang == LANG_JAVA) y &= w - 1;       /* Java masks the shift count */
        else if (y < 0 || (py ? 0 : y >= w)) {
            if (py) { snprintf(err, el, "ValueError: negative shift count"); return SEM_TRAP; }
            snprintf(err, el, "shift by %lld is undefined behaviour in C", y); return SEM_UB;
        }
        if (op == B_SHL) {
            if (py) { if (y >= 63 || (x != 0 && (llabs(x) >> (62 - y)) != 0)) return SEM_UNSUP; r = x * (1LL << y); }
            else if (lang == LANG_C && x < 0) { snprintf(err, el, "left shift of a negative value is undefined behaviour in C"); return SEM_UB; }
            else r = (long long)((unsigned long long)x << y);
        } else if (op == B_SHR) r = py && y >= 64 ? (x < 0 ? -1 : 0) : x >> y;
        else r = w == 32 ? (long long)((unsigned)x >> y) : (long long)((unsigned long long)x >> y);
        break;
    }
    case B_EQ: *out = lang == LANG_C ? v_int(x == y) : v_bool(x == y); return SEM_OK;
    case B_NE: *out = lang == LANG_C ? v_int(x != y) : v_bool(x != y); return SEM_OK;
    case B_LT: *out = lang == LANG_C ? v_int(x < y) : v_bool(x < y); return SEM_OK;
    case B_LE: *out = lang == LANG_C ? v_int(x <= y) : v_bool(x <= y); return SEM_OK;
    case B_GT: *out = lang == LANG_C ? v_int(x > y) : v_bool(x > y); return SEM_OK;
    case B_GE: *out = lang == LANG_C ? v_int(x >= y) : v_bool(x >= y); return SEM_OK;
    default: return SEM_UNSUP;
    }
    if (py) {
        if (ovf) { snprintf(err, el, "integer result exceeds 64 bits (Python's unbounded integers are not modelled)"); return SEM_UNSUP; }
        *out = v_int(r);
        return SEM_OK;
    }
    /* C / Java: reduce to the operation's width */
    long long fitted;
    int fits = fit(r, ty, &fitted);
    if (ty == TY_UINT) { *out = v_int(fitted); return SEM_OK; }          /* unsigned wraps in C */
    if (lang == LANG_C && (ovf || !fits)) {
        snprintf(err, el, "signed integer overflow in '%lld %s %lld' (undefined behaviour in C)", x, binop_sym(op), y);
        *out = v_int(fitted);
        return SEM_UB;
    }
    if (ovf && wide) {                                                    /* Java long wraps */
        unsigned long long ux = (unsigned long long)x, uy = (unsigned long long)y, ur = 0;
        if (op == B_ADD) ur = ux + uy; else if (op == B_SUB) ur = ux - uy; else if (op == B_MUL) ur = ux * uy; else ur = (unsigned long long)r;
        fitted = (long long)ur;
    }
    *out = v_int(fitted);
    return SEM_OK;
}

static SemResult dbl_op(Lang lang, BinOp op, double x, double y, int f32, Val *out, char *err, size_t el) {
    double r;
    int cmp = -1;
    switch (op) {
    case B_ADD: r = x + y; break;
    case B_SUB: r = x - y; break;
    case B_MUL: r = x * y; break;
    case B_DIV:
        if (y == 0 && lang == LANG_PY) { snprintf(err, el, "ZeroDivisionError: float division by zero"); return SEM_TRAP; }
        r = x / y; break;
    case B_MOD:
        if (y == 0 && lang == LANG_PY) { snprintf(err, el, "ZeroDivisionError: float modulo"); return SEM_TRAP; }
        r = fmod(x, y);
        if (lang == LANG_PY && r != 0 && ((r < 0) != (y < 0))) r += y;
        break;
    case B_FLOORDIV:
        if (y == 0) { snprintf(err, el, "ZeroDivisionError: float floor division by zero"); return SEM_TRAP; }
        r = floor(x / y); break;
    case B_POW:
        if (x == 0 && y < 0 && lang == LANG_PY) { snprintf(err, el, "ZeroDivisionError: 0.0 cannot be raised to a negative power"); return SEM_TRAP; }
        r = pow(x, y); break;
    case B_EQ: cmp = x == y; break;
    case B_NE: cmp = x != y; break;
    case B_LT: cmp = x < y; break;
    case B_LE: cmp = x <= y; break;
    case B_GT: cmp = x > y; break;
    case B_GE: cmp = x >= y; break;
    default: snprintf(err, el, "operator %s is not defined for floating-point values", binop_sym(op)); return lang == LANG_PY ? SEM_TRAP : SEM_UNSUP;
    }
    if (cmp >= 0) { *out = lang == LANG_C ? v_int(cmp) : v_bool(cmp); return SEM_OK; }
    if (f32) r = (float)r;
    *out = v_dbl(r);
    out->f32 = (unsigned char)f32;
    if (lang == LANG_PY && isinf(r) && !isinf(x) && !isinf(y)) { snprintf(err, el, "OverflowError: numerical result out of range"); return SEM_TRAP; }
    return SEM_OK;
}

static int seq_eq(const Val *a, const Val *b);

static int val_eq(const Val *a, const Val *b) {
    if (v_is_num(a) && v_is_num(b)) return (a->k == V_DBL || b->k == V_DBL) ? v_as_dbl(a) == v_as_dbl(b) : a->i == b->i;
    if (a->k != b->k) return 0;
    if (a->k == V_NONE) return 1;
    if (a->k == V_STR) return a->o->slen == b->o->slen && memcmp(a->o->s, b->o->s, a->o->slen) == 0;
    if (a->k == V_SEQ) return seq_eq(a, b);
    return 0;
}
static int seq_eq(const Val *a, const Val *b) {
    if (a->o == b->o) return 1;
    if (a->o->n != b->o->n || a->o->seq != b->o->seq) return 0;
    for (int i = 0; i < a->o->n; i++) if (!val_eq(&a->o->items[i], &b->o->items[i])) return 0;
    return 1;
}

static int str_cmp(const Val *a, const Val *b) {
    size_t n = a->o->slen < b->o->slen ? a->o->slen : b->o->slen;
    int c = memcmp(a->o->s, b->o->s, n);
    if (c) return c;
    return (a->o->slen > b->o->slen) - (a->o->slen < b->o->slen);
}

static const char *py_type(const Val *v) {
    switch (v->k) {
    case V_NONE: return "NoneType"; case V_BOOL: return "bool"; case V_INT: case V_CHAR: return "int";
    case V_DBL: return "float"; case V_STR: return "str";
    case V_SEQ: return v->o->seq == SEQ_TUPLE ? "tuple" : "list";
    default: return "undefined";
    }
}

static SemResult cmp_result(Lang lang, BinOp op, int c, Val *out) {
    int r = op == B_LT ? c < 0 : op == B_LE ? c <= 0 : op == B_GT ? c > 0 : op == B_GE ? c >= 0 : op == B_EQ ? c == 0 : c != 0;
    *out = lang == LANG_C ? v_int(r) : v_bool(r);
    return SEM_OK;
}

SemResult sem_bin(Lang lang, Ty ty, BinOp op, const Val *a, const Val *b, Val *out, Heap *h, char *err, size_t el) {
    err[0] = 0;
    if (a->k == V_UNDEF || b->k == V_UNDEF) { snprintf(err, el, "use of an uninitialized value"); return lang == LANG_C ? SEM_UB : SEM_TRAP; }

    /* identity and membership */
    if (op == B_IS || op == B_ISNOT) {
        int same = a->k == b->k && (a->k == V_NONE || (a->k >= V_STR ? a->o == b->o : val_eq(a, b)));
        *out = v_bool(op == B_IS ? same : !same);
        return SEM_OK;
    }
    if (op == B_IN || op == B_NOTIN) {
        int found = 0;
        if (b->k == V_STR && a->k == V_STR) {
            found = a->o->slen == 0;
            for (size_t i = 0; !found && i + a->o->slen <= b->o->slen; i++) found = memcmp(b->o->s + i, a->o->s, a->o->slen) == 0;
        } else if (b->k == V_SEQ) {
            for (int i = 0; !found && i < b->o->n; i++) found = val_eq(a, &b->o->items[i]);
        } else { snprintf(err, el, "TypeError: argument of type '%s' is not iterable", py_type(b)); return SEM_TRAP; }
        *out = v_bool(op == B_IN ? found : !found);
        return SEM_OK;
    }

    /* strings and sequences */
    if (a->k == V_STR || b->k == V_STR || a->k == V_SEQ || b->k == V_SEQ || a->k == V_NONE || b->k == V_NONE) {
        if (op == B_ADD && (ty == TY_STR || (lang == LANG_JAVA && (a->k == V_STR || b->k == V_STR)))) {   /* Java concat */
            Buf s = {0};
            v_tostr(&s, a, lang, 0); v_tostr(&s, b, lang, 0);
            *out = v_str(h, s.s ? s.s : "", s.n);
            free(s.s);
            return SEM_OK;
        }
        if (op == B_EQ || op == B_NE) {
            int eq = lang == LANG_JAVA && a->k >= V_STR && b->k >= V_STR ? a->o == b->o : val_eq(a, b);   /* Java ==: reference */
            if (lang == LANG_JAVA && (a->k == V_NONE || b->k == V_NONE)) eq = a->k == b->k;
            *out = lang == LANG_C ? v_int(op == B_EQ ? eq : !eq) : v_bool(op == B_EQ ? eq : !eq);
            return SEM_OK;
        }
        if (lang == LANG_PY) {
            if (a->k == V_STR && b->k == V_STR) {
                if (op == B_ADD) {
                    Val r = v_str(h, "", 0);
                    free(r.o->s);
                    r.o->slen = a->o->slen + b->o->slen;
                    r.o->s = xmalloc(r.o->slen + 1);
                    memcpy(r.o->s, a->o->s, a->o->slen); memcpy(r.o->s + a->o->slen, b->o->s, b->o->slen);
                    r.o->s[r.o->slen] = 0;
                    h->bytes += r.o->slen;
                    *out = r;
                    return SEM_OK;
                }
                if (op >= B_LT && op <= B_GE) return cmp_result(lang, op, str_cmp(a, b), out);
            }
            if (op == B_MUL && ((a->k == V_STR || a->k == V_SEQ) && (b->k == V_INT || b->k == V_BOOL))) {
                long long n = b->i < 0 ? 0 : b->i;
                if (a->k == V_STR) {
                    if ((double)n * (double)a->o->slen > 5e7) { snprintf(err, el, "string repetition too large for the interpreter"); return SEM_UNSUP; }
                    Buf s = {0};
                    for (long long i = 0; i < n; i++) bput(&s, a->o->s, a->o->slen);
                    *out = v_str(h, s.s ? s.s : "", s.n);
                    free(s.s);
                } else {
                    if ((double)n * a->o->n > 5e6) { snprintf(err, el, "list repetition too large for the interpreter"); return SEM_UNSUP; }
                    Val r = v_seq(h, (SeqKind)a->o->seq, (int)(n * a->o->n), TY_DYN);
                    for (long long i = 0; i < n; i++) for (int j = 0; j < a->o->n; j++) r.o->items[i * a->o->n + j] = a->o->items[j];
                    *out = r;
                }
                return SEM_OK;
            }
            if (op == B_MUL && (b->k == V_STR || b->k == V_SEQ) && (a->k == V_INT || a->k == V_BOOL)) return sem_bin(lang, ty, op, b, a, out, h, err, el);
            if (op == B_ADD && a->k == V_SEQ && b->k == V_SEQ && a->o->seq == b->o->seq) {
                Val r = v_seq(h, (SeqKind)a->o->seq, a->o->n + b->o->n, TY_DYN);
                for (int i = 0; i < a->o->n; i++) r.o->items[i] = a->o->items[i];
                for (int i = 0; i < b->o->n; i++) r.o->items[a->o->n + i] = b->o->items[i];
                *out = r;
                return SEM_OK;
            }
            if (op >= B_LT && op <= B_GE && a->k == V_SEQ && b->k == V_SEQ) {
                int n = a->o->n < b->o->n ? a->o->n : b->o->n;
                for (int i = 0; i < n; i++) {
                    if (val_eq(&a->o->items[i], &b->o->items[i])) continue;
                    Val r;
                    SemResult s = sem_bin(lang, ty, B_LT, &a->o->items[i], &b->o->items[i], &r, h, err, el);
                    if (s != SEM_OK) return s;
                    return cmp_result(lang, op, r.i ? -1 : 1, out);
                }
                return cmp_result(lang, op, (a->o->n > b->o->n) - (a->o->n < b->o->n), out);
            }
            snprintf(err, el, "TypeError: unsupported operand type(s) for %s: '%s' and '%s'", binop_sym(op), py_type(a), py_type(b));
            return SEM_TRAP;
        }
        snprintf(err, el, "operator %s is not modelled for these operand types", binop_sym(op));
        return SEM_UNSUP;
    }

    /* numbers */
    int is_dbl = a->k == V_DBL || b->k == V_DBL || (lang != LANG_PY && (ty == TY_DOUBLE || ty == TY_FLOAT) && op <= B_POW);
    if (lang == LANG_PY && op == B_DIV) is_dbl = 1;                     /* true division */
    if (lang == LANG_PY && op == B_POW && !is_dbl && b->i < 0) is_dbl = 1;
    if (is_dbl) {
        if (op >= B_AND && op <= B_USHR) {
            snprintf(err, el, "TypeError: unsupported operand type(s) for %s: 'float'", binop_sym(op));
            return lang == LANG_PY ? SEM_TRAP : SEM_UNSUP;
        }
        int f32 = lang != LANG_PY && ty == TY_FLOAT && (a->f32 || a->k != V_DBL) && (b->f32 || b->k != V_DBL);
        return dbl_op(lang, op, v_as_dbl(a), v_as_dbl(b), f32, out, err, el);
    }
    SemResult r = int_op(lang, ty == TY_LONG || ty == TY_UINT ? ty : TY_INT, op, a->i, b->i, out, err, el);
    if (lang == LANG_JAVA && r == SEM_OK && out->k == V_INT && ty == TY_CHAR) out->k = V_INT;
    return r;
}

SemResult sem_un(Lang lang, Ty ty, UnOp op, const Val *a, Val *out, Heap *h, char *err, size_t el) {
    err[0] = 0;
    if (a->k == V_UNDEF) { snprintf(err, el, "use of an uninitialized value"); return lang == LANG_C ? SEM_UB : SEM_TRAP; }
    switch (op) {
    case U_NOT: *out = lang == LANG_C ? v_int(!v_truthy(a)) : v_bool(!v_truthy(a)); return SEM_OK;
    case U_TOBOOL: *out = lang == LANG_C ? v_int(v_truthy(a)) : v_bool(v_truthy(a)); return SEM_OK;
    case U_TOSTR: case U_REPR: {
        Buf s = {0};
        v_tostr(&s, a, lang, op == U_REPR);
        *out = v_str(h, s.s ? s.s : "", s.n);
        free(s.s);
        return SEM_OK;
    }
    case U_LEN:
        if (a->k == V_STR) { *out = v_int((long long)a->o->slen); return SEM_OK; }
        if (a->k == V_SEQ) { *out = v_int(a->o->n); return SEM_OK; }
        snprintf(err, el, "TypeError: object of type '%s' has no len()", py_type(a));
        return SEM_TRAP;
    default: break;
    }
    if (!v_is_num(a)) {
        snprintf(err, el, "TypeError: bad operand type for unary %s: '%s'", unop_sym(op), py_type(a));
        return lang == LANG_PY ? SEM_TRAP : SEM_UNSUP;
    }
    switch (op) {
    case U_NEG:
        if (a->k == V_DBL) { *out = *a; out->d = -a->d; return SEM_OK; }
        { Val z = v_int(0); return sem_bin(lang, ty, B_SUB, &z, a, out, h, err, el); }
    case U_POS: *out = *a; if (out->k == V_BOOL || out->k == V_CHAR) out->k = V_INT; return SEM_OK;
    case U_BNOT:
        if (a->k == V_DBL) { snprintf(err, el, "TypeError: bad operand type for unary ~: 'float'"); return SEM_TRAP; }
        { long long r; fit(~a->i, lang == LANG_PY ? TY_LONG : ty, &r); *out = v_int(lang == LANG_PY ? ~a->i : r); return SEM_OK; }
    case U_TOINT: case U_TOLONG: {
        Ty t = op == U_TOLONG ? TY_LONG : (ty == TY_UINT ? TY_UINT : TY_INT);
        if (a->k == V_DBL) {
            double d = trunc(a->d);
            double lo = t == TY_LONG ? -9223372036854775808.0 : t == TY_UINT ? 0 : -2147483648.0;
            double hi = t == TY_LONG ? 9223372036854775807.0 : t == TY_UINT ? 4294967295.0 : 2147483647.0;
            if (isnan(d) || d < lo || d > hi) {
                if (lang == LANG_JAVA) { *out = v_int(isnan(d) ? 0 : d < lo ? (long long)lo : (long long)hi); return SEM_OK; }  /* saturates */
                if (lang == LANG_PY) {
                    if (isnan(d) || isinf(d)) { snprintf(err, el, "ValueError: cannot convert float %s to integer", isnan(d) ? "NaN" : "infinity"); return SEM_TRAP; }
                    if (fabs(d) >= 9.2e18) { snprintf(err, el, "integer result exceeds 64 bits (not modelled)"); return SEM_UNSUP; }
                    *out = v_int((long long)d); return SEM_OK;
                }
                snprintf(err, el, "converting %g to an integer is out of range (undefined behaviour in C)", a->d);
                *out = v_int(0);
                return SEM_UB;
            }
            *out = v_int((long long)d);
            return SEM_OK;
        }
        long long r;
        if (lang == LANG_PY) r = a->i; else fit(a->i, t, &r);
        *out = v_int(r);
        return SEM_OK;
    }
    case U_TODBL: case U_TOFLOAT: {
        double d = v_as_dbl(a);
        if (op == U_TOFLOAT) d = (float)d;
        *out = v_dbl(d);
        out->f32 = op == U_TOFLOAT;
        return SEM_OK;
    }
    case U_TOCHAR: {
        long long v = a->k == V_DBL ? (long long)a->d : a->i;
        *out = v_int(lang == LANG_JAVA ? (v & 0xFFFF) : (long long)(signed char)v);
        out->k = V_CHAR;
        return SEM_OK;
    }
    default: return SEM_UNSUP;
    }
}

Val opnd_val(const Opnd *o, Lang lang, Heap *h) {
    switch (o->k) {
    case OP_INT: {
        Val v = v_int(o->i);
        if (o->ty == TY_CHAR && lang != LANG_C) v.k = V_CHAR;
        return v;
    }
    case OP_BOOL: return lang == LANG_C ? v_int(o->i) : v_bool((int)o->i);
    case OP_FLT: { Val v = v_dbl(o->f); v.f32 = o->ty == TY_FLOAT; return v; }
    case OP_STR: return v_str(h, o->s, strlen(o->s));
    case OP_NULL: return v_none();
    default: { Val v = {0}; return v; }
    }
}

int val_opnd(const Val *v, Ty ty, Opnd *out) {
    switch (v->k) {
    case V_INT: *out = o_int(v->i, ty_is_int(ty) ? ty : (ty == TY_DYN ? TY_DYN : TY_INT)); if (ty == TY_CHAR) out->ty = TY_INT; return 1;
    case V_CHAR: *out = o_int(v->i, TY_CHAR); return 1;
    case V_BOOL: *out = o_bool((int)v->i); return 1;
    case V_DBL: *out = o_flt(v->d, v->f32 ? TY_FLOAT : TY_DOUBLE); return 1;
    case V_STR: if (memchr(v->o->s, 0, v->o->slen)) return 0; *out = o_str(intern_n(v->o->s, v->o->slen)); return 1;
    case V_NONE: *out = o_null(); return 1;
    default: return 0;
    }
}
