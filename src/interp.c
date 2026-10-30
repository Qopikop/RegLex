#include "interp.h"
#include "value.h"
#include <math.h>
#include <limits.h>
#include <ctype.h>
#include <pthread.h>

const char *run_status_name(RunStatus s) {
    static const char *n[] = { "ok", "exception", "unsupported", "limit" };
    return n[s];
}

/* ---- prepared code: operands resolved to frame slots / constants ---- */

typedef struct { unsigned char kind; int slot; Val k; } XOp;     /* kind: 0 none, 1 local, 2 global, 3 const */
typedef struct {
    unsigned char op, sub, ty;
    XOp d, a, b, c;
    int target, fn, nargs, line;
    const char *name;
    unsigned char dty, aty;
} XIns;
typedef struct { Func *f; XIns *code; int n, nslots; int *param_slots; int nparams; } XFunc;

enum {   /* builtins */
    BI_NONE = 0, BI_PRINT, BI_PRINTF, BI_PUTS, BI_PUTCHAR, BI_STRLEN, BI_ABS, BI_SQRT, BI_POW, BI_FABS, BI_FLOOR,
    BI_CEIL, BI_FMAX, BI_FMIN, BI_EXIT, BI_STRCMP, BI_ATOI, BI_ISDIGIT, BI_ISALPHA, BI_ISSPACE, BI_TOUPPER,
    BI_TOLOWER, BI_JPRINTLN, BI_JPRINT, BI_JPRINTF, BI_MAX, BI_MIN, BI_ROUND, BI_PARSEINT, BI_VALUEOF,
    BI_EQUALS, BI_SUBSTRING, BI_INDEXOF, BI_COMPARETO, BI_ISEMPTY, BI_CONTAINS, BI_UPPER, BI_LOWER,
    BI_ARR_TOSTRING, BI_ARR_SORT, BI_ARR_FILL, BI_LEN, BI_INT, BI_FLOAT, BI_STR, BI_BOOL, BI_SUM, BI_SORTED,
    BI_LIST, BI_TUPLE, BI_ORD, BI_CHR, BI_RANGE, BI_RANGE_CHECK, BI_SLICE, BI_FORMAT, BI_RAISE, BI_THROW,
    BI_ASSERT, BI_GCD, BI_PI, BI_E, BI_APPEND, BI_POP, BI_INSERT, BI_EXTEND, BI_REVERSE, BI_SORT, BI_INDEX,
    BI_COUNT, BI_JOIN, BI_STRIP, BI_SPLIT, BI_REPLACE, BI_STARTSWITH, BI_ENDSWITH, BI_FIND, BI_COPY,
    BI_DIVMOD, BI_ZIP, BI_ENUMERATE, BI_REVERSED, BI_ANY, BI_ALL, BI_EXC, BI_JEXC, BI_ISDIGIT_PY, BI_ISALPHA_PY,
    BI_CHAR_ISDIGIT, BI_CHAR_ISLETTER, BI_SYSEXIT, BI_STARTSWITH_J, BI_PYPOW, BI_TOSTRING_INT, BI_LSTRIP, BI_RSTRIP
};

typedef struct {
    IRProg *P;
    Lang lang;
    XFunc *fx;
    int nf;
    Val *globals;
    int nglobals;
    const char **gnames;
    Heap heap;
    Heap cheap;               /* constants */
    VEC(Val) args;
    RunResult *r;
    const RunLimits *lim;
    int depth;
    int stop;                 /* set when the program must end (exception, limit, exit) */
    VEC(Obj *) str_cache_obj;
    VEC(const char *) str_cache_key;
} VM;

static void stop_with(VM *vm, RunStatus st, const char *fmt, ...) {
    if (vm->stop) return;
    vm->stop = 1;
    vm->r->status = st;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->r->msg, sizeof(vm->r->msg), fmt, ap);
    va_end(ap);
}

static int global_slot(VM *vm, const char *name) {
    for (int i = 0; i < vm->nglobals; i++) if (vm->gnames[i] == name) return i;
    vm->gnames = xrealloc((void *)vm->gnames, sizeof(char *) * (size_t)(vm->nglobals + 1));
    vm->globals = xrealloc(vm->globals, sizeof(Val) * (size_t)(vm->nglobals + 1));
    vm->gnames[vm->nglobals] = name;
    memset(&vm->globals[vm->nglobals], 0, sizeof(Val));    /* V_UNDEF until assigned */
    return vm->nglobals++;
}

static Val const_val(VM *vm, const Opnd *o) {
    if (o->k == OP_STR) {
        for (int i = 0; i < vm->str_cache_key.n; i++)
            if (vm->str_cache_key.v[i] == o->s) { Val v = {0}; v.k = V_STR; v.o = vm->str_cache_obj.v[i]; return v; }
        Val v = v_str(&vm->cheap, o->s, strlen(o->s));
        vpush(vm->str_cache_key, o->s);
        vpush(vm->str_cache_obj, v.o);
        return v;
    }
    return opnd_val(o, vm->lang, &vm->cheap);
}

static int builtin_id(Lang lang, const char *name) {
    static const struct { int l; const char *n; int id; } t[] = {   /* l: -1 any */
        {LANG_C, "printf", BI_PRINTF}, {LANG_C, "puts", BI_PUTS}, {LANG_C, "putchar", BI_PUTCHAR}, {LANG_C, "strlen", BI_STRLEN},
        {LANG_C, "abs", BI_ABS}, {LANG_C, "labs", BI_ABS}, {-1, "sqrt", BI_SQRT}, {LANG_C, "pow", BI_POW}, {LANG_C, "fabs", BI_FABS},
        {LANG_C, "floor", BI_FLOOR}, {LANG_C, "ceil", BI_CEIL}, {LANG_C, "fmax", BI_FMAX}, {LANG_C, "fmin", BI_FMIN},
        {LANG_C, "exit", BI_EXIT}, {LANG_C, "strcmp", BI_STRCMP}, {LANG_C, "atoi", BI_ATOI}, {LANG_C, "isdigit", BI_ISDIGIT},
        {LANG_C, "isalpha", BI_ISALPHA}, {LANG_C, "isspace", BI_ISSPACE}, {LANG_C, "toupper", BI_TOUPPER}, {LANG_C, "tolower", BI_TOLOWER},
        {LANG_JAVA, "System.out.println", BI_JPRINTLN}, {LANG_JAVA, "System.out.print", BI_JPRINT}, {LANG_JAVA, "System.out.printf", BI_JPRINTF},
        {LANG_JAVA, "String.format", BI_JPRINTF},
        {LANG_JAVA, "Math.max", BI_MAX}, {LANG_JAVA, "Math.min", BI_MIN}, {LANG_JAVA, "Math.abs", BI_ABS}, {LANG_JAVA, "Math.sqrt", BI_SQRT},
        {LANG_JAVA, "Math.pow", BI_POW}, {LANG_JAVA, "Math.floor", BI_FLOOR}, {LANG_JAVA, "Math.ceil", BI_CEIL}, {LANG_JAVA, "Math.round", BI_ROUND},
        {LANG_JAVA, "Integer.parseInt", BI_PARSEINT}, {LANG_JAVA, "String.valueOf", BI_VALUEOF}, {LANG_JAVA, "Integer.toString", BI_VALUEOF},
        {LANG_JAVA, "String.equals", BI_EQUALS}, {LANG_JAVA, "String.substring", BI_SUBSTRING}, {LANG_JAVA, "String.indexOf", BI_INDEXOF},
        {LANG_JAVA, "String.compareTo", BI_COMPARETO}, {LANG_JAVA, "String.isEmpty", BI_ISEMPTY}, {LANG_JAVA, "String.contains", BI_CONTAINS},
        {LANG_JAVA, "String.toUpperCase", BI_UPPER}, {LANG_JAVA, "String.toLowerCase", BI_LOWER}, {LANG_JAVA, "String.startsWith", BI_STARTSWITH_J},
        {LANG_JAVA, "Arrays.toString", BI_ARR_TOSTRING}, {LANG_JAVA, "Arrays.sort", BI_ARR_SORT}, {LANG_JAVA, "Arrays.fill", BI_ARR_FILL},
        {LANG_JAVA, "Character.isDigit", BI_CHAR_ISDIGIT}, {LANG_JAVA, "Character.isLetter", BI_CHAR_ISLETTER}, {LANG_JAVA, "System.exit", BI_SYSEXIT},
        {LANG_JAVA, "String.length", BI_LEN}, {LANG_JAVA, "throw", BI_THROW},
        {LANG_PY, "len", BI_LEN}, {LANG_PY, "abs", BI_ABS}, {LANG_PY, "min", BI_MIN}, {LANG_PY, "max", BI_MAX}, {LANG_PY, "int", BI_INT},
        {LANG_PY, "float", BI_FLOAT}, {LANG_PY, "str", BI_STR}, {LANG_PY, "bool", BI_BOOL}, {LANG_PY, "sum", BI_SUM}, {LANG_PY, "sorted", BI_SORTED},
        {LANG_PY, "list", BI_LIST}, {LANG_PY, "tuple", BI_TUPLE}, {LANG_PY, "round", BI_ROUND}, {LANG_PY, "ord", BI_ORD}, {LANG_PY, "chr", BI_CHR},
        {LANG_PY, "range", BI_RANGE}, {LANG_PY, "range_check", BI_RANGE_CHECK}, {LANG_PY, "slice", BI_SLICE}, {LANG_PY, "format", BI_FORMAT},
        {LANG_PY, "raise", BI_RAISE}, {-1, "assert_fail", BI_ASSERT}, {LANG_PY, "math.sqrt", BI_SQRT}, {LANG_PY, "math.floor", BI_FLOOR},
        {LANG_PY, "math.ceil", BI_CEIL}, {LANG_PY, "math.gcd", BI_GCD}, {LANG_PY, "math.pi", BI_PI}, {LANG_PY, "math.e", BI_E},
        {LANG_PY, "math.pow", BI_POW}, {LANG_PY, "math.fabs", BI_FABS},
        {LANG_PY, ".append", BI_APPEND}, {LANG_PY, ".pop", BI_POP}, {LANG_PY, ".insert", BI_INSERT}, {LANG_PY, ".extend", BI_EXTEND},
        {LANG_PY, ".reverse", BI_REVERSE}, {LANG_PY, ".sort", BI_SORT}, {LANG_PY, ".index", BI_INDEX}, {LANG_PY, ".count", BI_COUNT},
        {LANG_PY, ".join", BI_JOIN}, {LANG_PY, ".upper", BI_UPPER}, {LANG_PY, ".lower", BI_LOWER}, {LANG_PY, ".strip", BI_STRIP}, {LANG_PY, ".lstrip", BI_LSTRIP}, {LANG_PY, ".rstrip", BI_RSTRIP},
        {LANG_PY, ".split", BI_SPLIT}, {LANG_PY, ".replace", BI_REPLACE}, {LANG_PY, ".startswith", BI_STARTSWITH}, {LANG_PY, ".endswith", BI_ENDSWITH},
        {LANG_PY, ".find", BI_FIND}, {LANG_PY, ".copy", BI_COPY}, {LANG_PY, ".isdigit", BI_ISDIGIT_PY}, {LANG_PY, ".isalpha", BI_ISALPHA_PY},
        {LANG_PY, "divmod", BI_DIVMOD}, {LANG_PY, "zip", BI_ZIP}, {LANG_PY, "enumerate", BI_ENUMERATE}, {LANG_PY, "reversed", BI_REVERSED},
        {LANG_PY, "any", BI_ANY}, {LANG_PY, "all", BI_ALL}, {LANG_PY, "pow", BI_PYPOW},
    };
    char base[128];
    snprintf(base, sizeof(base), "%s", name);
    if (lang == LANG_PY) { char *c = strchr(base + 1, ':'); if (c) *c = 0; }         /* print:sep:end -> print */
    if (lang == LANG_PY && !strcmp(base, "print")) return BI_PRINT;
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if ((t[i].l < 0 || t[i].l == (int)lang) && !strcmp(t[i].n, base)) return t[i].id;
    if (lang == LANG_PY && (strstr(base, "Error") || strstr(base, "Exception")) && !strchr(base, '.')) return BI_EXC;
    if (lang == LANG_JAVA && !strncmp(base, "new ", 4) && (strstr(base, "Exception") || strstr(base, "Error"))) return BI_JEXC;
    return BI_NONE;
}

static void prepare(VM *vm) {
    IRProg *P = vm->P;
    vm->nf = P->f.n;
    vm->fx = xcalloc((size_t)(vm->nf ? vm->nf : 1), sizeof(XFunc));
    for (int fi = 0; fi < P->f.n; fi++) {
        Func *f = &P->f.v[fi];
        XFunc *x = &vm->fx[fi];
        x->f = f;
        x->n = f->code.n;
        x->code = xcalloc((size_t)(x->n ? x->n : 1), sizeof(XIns));
        /* local slots: parameters first, then every local variable and temp */
        VEC(const char *) names = {0};
        IntVec temps = {0};
        for (int i = 0; i < f->params.n; i++) vpush(names, f->params.v[i]);
        x->nparams = f->params.n;
#define SLOT(o, out) do { \
            const Opnd *o_ = (o); XOp *r_ = (out); memset(r_, 0, sizeof(*r_)); \
            if (o_->k == OP_NONE) break; \
            if (o_const(o_)) { r_->kind = 3; r_->k = const_val(vm, o_); break; } \
            if (o_->k == OP_VAR && o_->g) { r_->kind = 2; r_->slot = global_slot(vm, o_->s); break; } \
            r_->kind = 1; \
            if (o_->k == OP_VAR) { int s_ = -1; for (int q_ = 0; q_ < names.n; q_++) if (names.v[q_] == o_->s) { s_ = q_; break; } \
                if (s_ < 0) { vpush(names, o_->s); s_ = names.n - 1; } r_->slot = s_; } \
            else { int s_ = -1; for (int q_ = 0; q_ < temps.n; q_ += 2) if (temps.v[q_] == o_->id) { s_ = temps.v[q_ + 1]; break; } \
                if (s_ < 0) { s_ = 1000000 + temps.n / 2; vpush(temps, o_->id); vpush(temps, s_); } r_->slot = s_; } \
        } while (0)
        int maxlab = 0;
        for (int i = 0; i < f->code.n; i++) if (f->code.v[i].op == I_LABEL && f->code.v[i].lab > maxlab) maxlab = f->code.v[i].lab;
        int *labpos = xmalloc(sizeof(int) * (size_t)(maxlab + 1));
        for (int i = 0; i <= maxlab; i++) labpos[i] = -1;
        for (int i = 0; i < f->code.n; i++) if (f->code.v[i].op == I_LABEL) labpos[f->code.v[i].lab] = i;
        for (int i = 0; i < f->code.n; i++) {
            Instr *in = &f->code.v[i];
            XIns *xi = &x->code[i];
            xi->op = in->op; xi->sub = in->sub; xi->ty = in->ty; xi->line = in->line; xi->nargs = in->nargs; xi->name = in->fn;
            xi->dty = in->d.ty; xi->aty = in->a.ty;
            SLOT(&in->d, &xi->d); SLOT(&in->a, &xi->a); SLOT(&in->b, &xi->b); SLOT(&in->c, &xi->c);
            if (in->op == I_GOTO || in->op == I_IF || in->op == I_IFNOT) xi->target = in->lab <= maxlab ? labpos[in->lab] : -1;
            if (in->op == I_CALL) {
                xi->fn = -1;
                for (int k = 0; k < P->f.n; k++) if (P->f.v[k].name == in->fn || !strcmp(P->f.v[k].name, in->fn)) { xi->fn = k; break; }
                if (xi->fn < 0) xi->fn = -1 - builtin_id(vm->lang, in->fn);
            }
        }
#undef SLOT
        /* temps were numbered from 1000000: renumber after the named locals */
        x->nslots = names.n + temps.n / 2;
        for (int i = 0; i < x->n; i++) {
            XOp *ops[4] = { &x->code[i].d, &x->code[i].a, &x->code[i].b, &x->code[i].c };
            for (int k = 0; k < 4; k++) if (ops[k]->kind == 1 && ops[k]->slot >= 1000000) ops[k]->slot = ops[k]->slot - 1000000 + names.n;
        }
        x->param_slots = xmalloc(sizeof(int) * (size_t)(f->params.n ? f->params.n : 1));
        for (int i = 0; i < f->params.n; i++) x->param_slots[i] = i;
        free(labpos);
        vfree(names); vfree(temps);
    }
}

/* ---- output helpers ---- */

static void out_put(VM *vm, const char *s, size_t n) {
    if (vm->r->out.n + n > vm->lim->max_out) { stop_with(vm, RUN_LIMIT, "output limit of %zu bytes reached", vm->lim->max_out); return; }
    bput(&vm->r->out, s, n);
}
static void out_str(VM *vm, const char *s) { out_put(vm, s, strlen(s)); }
static void out_val(VM *vm, const Val *v, int repr) { Buf b = {0}; v_tostr(&b, v, vm->lang, repr); out_put(vm, b.s ? b.s : "", b.n); free(b.s); }

static long long as_int(const Val *v) { return v->k == V_DBL ? (long long)v->d : v->i; }

/* printf-style formatting for C printf and Java printf/String.format */
static void c_format(VM *vm, Buf *b, const Val *fmt, const Val *args, int n) {
    if (fmt->k != V_STR) { stop_with(vm, RUN_UNSUPPORTED, "printf with a non-literal format"); return; }
    const char *f = fmt->o->s;
    int ai = 0;
    for (size_t i = 0; i < fmt->o->slen; i++) {
        if (f[i] != '%') { bputc(b, f[i]); continue; }
        char spec[40];
        int k = 0;
        spec[k++] = '%';
        i++;
        while (i < fmt->o->slen && strchr("-+ #0,", f[i]) && k < 30) { if (f[i] != ',') spec[k++] = f[i]; i++; }
        while (i < fmt->o->slen && (isdigit((unsigned char)f[i]) || f[i] == '.') && k < 30) spec[k++] = f[i++];
        while (i < fmt->o->slen && strchr("hlLqjzt", f[i])) i++;
        if (i >= fmt->o->slen) break;
        char c = f[i];
        if (c == '%') { bputc(b, '%'); continue; }
        if (c == 'n' && vm->lang == LANG_JAVA) { bputc(b, '\n'); continue; }
        if (ai >= n) { if (vm->lang == LANG_JAVA) stop_with(vm, RUN_EXCEPTION, "java.util.MissingFormatArgumentException: Format specifier '%%%c'", c); else stop_with(vm, RUN_UNSUPPORTED, "printf has fewer arguments than conversions (undefined behaviour)"); return; }
        const Val *a = &args[ai++];
        char tmp[512];
        switch (c) {
        case 'd': case 'i':
            if (a->k == V_DBL && vm->lang == LANG_JAVA) { stop_with(vm, RUN_EXCEPTION, "java.util.IllegalFormatConversionException: d != java.lang.Double"); return; }
            spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = 'd'; spec[k] = 0;
            snprintf(tmp, sizeof(tmp), spec, as_int(a)); bputs(b, tmp); break;
        case 'u': case 'x': case 'X': case 'o':
            spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = c; spec[k] = 0;
            snprintf(tmp, sizeof(tmp), spec, (unsigned long long)(vm->lang == LANG_C ? (unsigned)as_int(a) : (unsigned long long)as_int(a))); bputs(b, tmp); break;
        case 'c':
            spec[k++] = 'c'; spec[k] = 0;
            snprintf(tmp, sizeof(tmp), spec, (int)as_int(a)); bputs(b, tmp); break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            spec[k++] = c; spec[k] = 0;
            snprintf(tmp, sizeof(tmp), spec, v_as_dbl(a)); bputs(b, tmp); break;
        case 's': case 'b': {
            Buf s = {0};
            if (c == 'b') bputs(&s, v_truthy(a) ? "true" : "false");
            else if (a->k == V_SEQ && vm->lang == LANG_C)          /* char array: up to the terminating 0 */
                for (int q = 0; q < a->o->n && a->o->items[q].i; q++) bputc(&s, (char)a->o->items[q].i);
            else v_tostr(&s, a, vm->lang, 0);
            spec[k++] = 's'; spec[k] = 0;
            if (k == 2) bputs(b, s.s ? s.s : ""); else { snprintf(tmp, sizeof(tmp), spec, s.s ? s.s : ""); bputs(b, tmp); }
            free(s.s);
            break;
        }
        default: stop_with(vm, RUN_UNSUPPORTED, "printf conversion '%%%c' is not modelled", c); return;
        }
    }
}

/* Python format spec: [[fill]align][sign][0][width][,][.prec][type] */
static int py_format(VM *vm, Buf *b, const Val *v, const char *spec) {
    char fill = ' ', align = 0, sign = '-', type = 0;
    int width = 0, prec = -1, comma = 0, zero = 0;
    const char *s = spec;
    if (s[0] && s[1] && strchr("<>^=", s[1])) { fill = s[0]; align = s[1]; s += 2; }
    else if (s[0] && strchr("<>^=", s[0])) { align = s[0]; s++; }
    if (*s && strchr("+- ", *s)) sign = *s++;
    if (*s == '0') { zero = 1; s++; }
    while (isdigit((unsigned char)*s)) width = width * 10 + (*s++ - '0');
    if (*s == ',' || *s == '_') { comma = 1; s++; }
    if (*s == '.') { s++; prec = 0; while (isdigit((unsigned char)*s)) prec = prec * 10 + (*s++ - '0'); }
    if (*s) type = *s++;
    if (*s) return 0;
    Buf body = {0};
    char tmp[512];
    if (type == 'f' || type == 'F' || type == 'e' || type == 'E' || type == '%' || (type == 'g' && v_is_num(v))) {
        if (!v_is_num(v)) { free(body.s); return 0; }
        double d = v_as_dbl(v) * (type == '%' ? 100 : 1);
        char f[16]; snprintf(f, sizeof(f), "%%.%d%c", prec < 0 ? 6 : prec, type == '%' ? 'f' : type);
        snprintf(tmp, sizeof(tmp), f, d);
        bputs(&body, tmp);
        if (type == '%') bputc(&body, '%');
    } else if (type == 'd' || type == 'x' || type == 'X' || type == 'b' || type == 'o') {
        if (v->k != V_INT && v->k != V_BOOL) { free(body.s); return 0; }
        if (type == 'b') { unsigned long long u = (unsigned long long)llabs(v->i); char bits[70]; int n = 0; do { bits[n++] = (char)('0' + (u & 1)); u >>= 1; } while (u); if (v->i < 0) bputc(&body, '-'); while (n) bputc(&body, bits[--n]); }
        else { snprintf(tmp, sizeof(tmp), type == 'd' ? "%lld" : type == 'x' ? "%llx" : type == 'X' ? "%llX" : "%llo", v->i); bputs(&body, tmp); }
    } else if (!type || type == 's' || type == 'g') {
        v_tostr(&body, v, vm->lang, 0);
        if (prec >= 0 && v->k == V_STR && (size_t)prec < body.n) { body.n = (size_t)prec; body.s[prec] = 0; }
        else if (prec >= 0 && v->k == V_DBL) { body.n = 0; snprintf(tmp, sizeof(tmp), "%.*g", prec, v->d); bputs(&body, tmp); }
    } else { free(body.s); return 0; }
    if (comma && body.s) {
        Buf c = {0};
        char *start = body.s + (body.s[0] == '-');
        size_t intlen = strcspn(start, ".eE%");
        bput(&c, body.s, (size_t)(start - body.s));
        for (size_t i = 0; i < intlen; i++) { bputc(&c, start[i]); if ((intlen - i - 1) % 3 == 0 && i + 1 < intlen) bputc(&c, ','); }
        bputs(&c, start + intlen);
        free(body.s); body = c;
    }
    if (sign == '+' && v_is_num(v) && body.s && body.s[0] != '-') { Buf c = {0}; bputc(&c, '+'); bputs(&c, body.s); free(body.s); body = c; }
    int len = (int)body.n;
    if (!align) align = (v_is_num(v) && type != 's') ? '>' : '<';
    if (zero && v_is_num(v)) { fill = '0'; align = '='; }
    int pad = width > len ? width - len : 0;
    if (align == '=' && body.s && (body.s[0] == '-' || body.s[0] == '+')) { bputc(b, body.s[0]); for (int i = 0; i < pad; i++) bputc(b, fill); bputs(b, body.s + 1); }
    else if (align == '>' || align == '=') { for (int i = 0; i < pad; i++) bputc(b, fill); bputs(b, body.s ? body.s : ""); }
    else if (align == '^') { for (int i = 0; i < pad / 2; i++) bputc(b, fill); bputs(b, body.s ? body.s : ""); for (int i = 0; i < pad - pad / 2; i++) bputc(b, fill); }
    else { bputs(b, body.s ? body.s : ""); for (int i = 0; i < pad; i++) bputc(b, fill); }
    free(body.s);
    return 1;
}

/* ---- sequences ---- */

static int norm_index(VM *vm, const Val *seq, long long idx, long long n, int *ok) {
    *ok = 1;
    if (vm->lang == LANG_PY && idx < 0) idx += n;
    if (idx < 0 || idx >= n) {
        *ok = 0;
        if (vm->lang == LANG_PY) stop_with(vm, RUN_EXCEPTION, "IndexError: %s index out of range", seq->k == V_STR ? "string" : "list");
        else if (vm->lang == LANG_JAVA) stop_with(vm, RUN_EXCEPTION, "java.lang.%sIndexOutOfBoundsException: Index %lld out of bounds for length %lld", seq->k == V_STR ? "String" : "Array", idx, n);
        else stop_with(vm, RUN_EXCEPTION, "array index %lld out of bounds [0, %lld) (undefined behaviour in C)", idx, n);
    }
    return (int)idx;
}

static void seq_push(VM *vm, Obj *o, Val v) {
    if (o->n == o->cap) { o->cap = o->cap ? o->cap * 2 : 4; o->items = xrealloc(o->items, sizeof(Val) * (size_t)o->cap); vm->heap.bytes += sizeof(Val) * (size_t)o->cap / 2; }
    o->items[o->n++] = v;
}

static Val to_list(VM *vm, const Val *v, SeqKind kind, int *ok) {
    *ok = 1;
    if (v->k == V_SEQ) {
        Val r = v_seq(&vm->heap, kind, v->o->n, TY_DYN);
        memcpy(r.o->items, v->o->items, sizeof(Val) * (size_t)v->o->n);
        return r;
    }
    if (v->k == V_STR) {
        Val r = v_seq(&vm->heap, kind, (int)v->o->slen, TY_DYN);
        for (size_t i = 0; i < v->o->slen; i++) r.o->items[i] = v_str(&vm->heap, v->o->s + i, 1);
        return r;
    }
    *ok = 0;
    stop_with(vm, RUN_EXCEPTION, "TypeError: object is not iterable");
    return v_none();
}

static int val_cmp(VM *vm, const Val *a, const Val *b) {
    Val r;
    char err[200];
    if (sem_bin(vm->lang, TY_DYN, B_LT, a, b, &r, &vm->heap, err, sizeof(err)) != SEM_OK) { stop_with(vm, RUN_EXCEPTION, "%s", err); return 0; }
    if (v_truthy(&r)) return -1;
    if (sem_bin(vm->lang, TY_DYN, B_LT, b, a, &r, &vm->heap, err, sizeof(err)) != SEM_OK) { stop_with(vm, RUN_EXCEPTION, "%s", err); return 0; }
    return v_truthy(&r) ? 1 : 0;
}

static void sort_vals(VM *vm, Val *v, int n) {
    /* insertion sort for small, merge sort otherwise (stable, like Python's and Java's object sort) */
    if (n < 2 || vm->stop) return;
    if (n <= 16) {
        for (int i = 1; i < n && !vm->stop; i++) { Val x = v[i]; int j = i - 1; while (j >= 0 && val_cmp(vm, &v[j], &x) > 0) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
        return;
    }
    int m = n / 2;
    sort_vals(vm, v, m); sort_vals(vm, v + m, n - m);
    Val *tmp = xmalloc(sizeof(Val) * (size_t)n);
    int i = 0, j = m, k = 0;
    while (i < m && j < n) tmp[k++] = val_cmp(vm, &v[j], &v[i]) < 0 ? v[j++] : v[i++];
    while (i < m) tmp[k++] = v[i++];
    while (j < n) tmp[k++] = v[j++];
    memcpy(v, tmp, sizeof(Val) * (size_t)n);
    free(tmp);
}

static Val mk_str(VM *vm, Buf *b) { Val v = v_str(&vm->heap, b->s ? b->s : "", b->n); free(b->s); b->s = NULL; b->n = b->cap = 0; return v; }

/* ---- builtin calls ---- */

static Val call_builtin(VM *vm, int id, const char *name, Val *a, int n) {
    Val none = v_none();
    Buf b = {0};
    char err[256];
    switch (id) {
    case BI_PRINT: {
        const char *sep = " ", *end = "\n";
        /* print:sep:end -- keyword arguments come after the positional ones, in name order */
        int npos = n;
        const char *kw = strchr(name, ':');
        int nk = 0;
        for (const char *c = kw; c; c = strchr(c + 1, ':')) nk++;
        npos = n - nk;
        const char *c = kw;
        for (int k = 0; k < nk; k++) {
            const Val *v = &a[npos + k];
            const char *nm = c + 1;
            size_t len = strcspn(nm, ":");
            if (v->k == V_STR && !strncmp(nm, "sep", len) && len == 3) sep = v->o->s;
            else if (v->k == V_STR && !strncmp(nm, "end", len) && len == 3) end = v->o->s;
            else if (v->k != V_NONE) { stop_with(vm, RUN_UNSUPPORTED, "print() keyword argument '%.*s' is not modelled", (int)len, nm); return none; }
            c = strchr(c + 1, ':');
        }
        for (int i = 0; i < npos; i++) { if (i) out_str(vm, sep); out_val(vm, &a[i], 0); }
        out_str(vm, end);
        return none;
    }
    case BI_JPRINTLN: case BI_JPRINT:
        if (n > 1) { stop_with(vm, RUN_UNSUPPORTED, "println with %d arguments", n); return none; }
        if (n) {
            if (a[0].k == V_SEQ && a[0].o->seq == SEQ_ARRAY && a[0].o->elem == TY_CHAR) { for (int i = 0; i < a[0].o->n; i++) out_val(vm, &a[0].o->items[i], 0); }
            else out_val(vm, &a[0], 0);
        }
        if (id == BI_JPRINTLN) out_str(vm, "\n");
        return none;
    case BI_PRINTF: case BI_JPRINTF:
        if (!n) return none;
        c_format(vm, &b, &a[0], a + 1, n - 1);
        if (!strcmp(name, "String.format")) return mk_str(vm, &b);
        out_put(vm, b.s ? b.s : "", b.n);
        free(b.s);
        return id == BI_PRINTF ? v_int((long long)b.n) : none;
    case BI_PUTS:
        if (n && a[0].k == V_STR) out_put(vm, a[0].o->s, a[0].o->slen);
        else if (n && a[0].k == V_SEQ) for (int q = 0; q < a[0].o->n && a[0].o->items[q].i; q++) { char ch = (char)a[0].o->items[q].i; out_put(vm, &ch, 1); }
        out_str(vm, "\n");
        return v_int(1);
    case BI_PUTCHAR: if (n) { char ch = (char)as_int(&a[0]); out_put(vm, &ch, 1); } return n ? v_int(as_int(&a[0])) : none;
    case BI_STRLEN:
        if (n && a[0].k == V_STR) return v_int((long long)strlen(a[0].o->s));
        if (n && a[0].k == V_SEQ) { int q = 0; while (q < a[0].o->n && a[0].o->items[q].i) q++; return v_int(q); }
        break;
    case BI_LEN: {
        if (n != 1) break;
        Val r;
        if (sem_un(vm->lang, TY_DYN, U_LEN, &a[0], &r, &vm->heap, err, sizeof(err)) != SEM_OK) { stop_with(vm, RUN_EXCEPTION, "%s", err); return none; }
        return r;
    }
    case BI_ABS:
        if (n != 1) break;
        if (a[0].k == V_DBL) { Val r = a[0]; r.d = fabs(r.d); return r; }
        if (vm->lang == LANG_C && a[0].i == INT_MIN) vm->r->ub_count++;
        return v_int(llabs(a[0].i));
    case BI_SQRT: if (n == 1 && v_is_num(&a[0])) { if (vm->lang == LANG_PY && v_as_dbl(&a[0]) < 0) { stop_with(vm, RUN_EXCEPTION, "ValueError: math domain error"); return none; } return v_dbl(sqrt(v_as_dbl(&a[0]))); } break;
    case BI_POW: if (n == 2) return v_dbl(pow(v_as_dbl(&a[0]), v_as_dbl(&a[1]))); break;
    case BI_PYPOW: if (n == 2) { Val r; SemResult s = sem_bin(vm->lang, TY_DYN, B_POW, &a[0], &a[1], &r, &vm->heap, err, sizeof(err)); if (s != SEM_OK) { stop_with(vm, s == SEM_TRAP ? RUN_EXCEPTION : RUN_UNSUPPORTED, "%s", err); return none; } return r; } break;
    case BI_FABS: if (n == 1) return v_dbl(fabs(v_as_dbl(&a[0]))); break;
    case BI_FLOOR: case BI_CEIL:
        if (n != 1) break;
        { double d = id == BI_FLOOR ? floor(v_as_dbl(&a[0])) : ceil(v_as_dbl(&a[0])); return vm->lang == LANG_PY ? v_int((long long)d) : v_dbl(d); }
    case BI_FMAX: if (n == 2) return v_dbl(fmax(v_as_dbl(&a[0]), v_as_dbl(&a[1]))); break;
    case BI_FMIN: if (n == 2) return v_dbl(fmin(v_as_dbl(&a[0]), v_as_dbl(&a[1]))); break;
    case BI_EXIT: case BI_SYSEXIT: vm->r->exit_code = n ? (int)as_int(&a[0]) : 0; vm->stop = 1; vm->r->status = RUN_OK; snprintf(vm->r->msg, sizeof(vm->r->msg), "exit(%d)", vm->r->exit_code); return none;
    case BI_STRCMP: if (n == 2 && a[0].k == V_STR && a[1].k == V_STR) { int c = strcmp(a[0].o->s, a[1].o->s); return v_int(c < 0 ? -1 : c > 0); } break;
    case BI_ATOI: if (n == 1 && a[0].k == V_STR) return v_int(atoi(a[0].o->s)); break;
    case BI_ISDIGIT: if (n == 1) return v_int(isdigit((int)as_int(&a[0])) ? 1 : 0); break;
    case BI_ISALPHA: if (n == 1) return v_int(isalpha((int)as_int(&a[0])) ? 1 : 0); break;
    case BI_ISSPACE: if (n == 1) return v_int(isspace((int)as_int(&a[0])) ? 1 : 0); break;
    case BI_TOUPPER: case BI_TOLOWER:
        if (n == 1 && a[0].k != V_STR) return v_int(id == BI_TOUPPER ? toupper((int)as_int(&a[0])) : tolower((int)as_int(&a[0])));
        break;
    case BI_CHAR_ISDIGIT: if (n == 1) return v_bool(isdigit((int)as_int(&a[0]))); break;
    case BI_CHAR_ISLETTER: if (n == 1) return v_bool(isalpha((int)as_int(&a[0]))); break;
    case BI_MAX: case BI_MIN: {
        Val *items = a; int cnt = n;
        if (n == 1 && a[0].k == V_SEQ) { items = a[0].o->items; cnt = a[0].o->n; }
        else if (n == 1 && a[0].k == V_STR) { int ok; Val l = to_list(vm, &a[0], SEQ_LIST, &ok); items = l.o->items; cnt = l.o->n; }
        if (!cnt) { stop_with(vm, RUN_EXCEPTION, "ValueError: %s() arg is an empty sequence", id == BI_MAX ? "max" : "min"); return none; }
        Val best = items[0];
        for (int i = 1; i < cnt && !vm->stop; i++) { int c = val_cmp(vm, &items[i], &best); if (id == BI_MAX ? c > 0 : c < 0) best = items[i]; }
        if (vm->lang == LANG_JAVA && n == 2 && (a[0].k == V_DBL || a[1].k == V_DBL)) { best = v_dbl(v_as_dbl(&best)); }
        return best;
    }
    case BI_ROUND:
        if (vm->lang == LANG_JAVA && n == 1) return v_int((long long)floor(v_as_dbl(&a[0]) + 0.5));
        if (n == 1) { if (a[0].k != V_DBL) return v_int(a[0].i); return v_int((long long)nearbyint(a[0].d)); }
        if (n == 2 && a[1].k != V_NONE) {
            if (a[0].k != V_DBL) return a[0];
            char t[64]; snprintf(t, sizeof(t), "%.*f", (int)as_int(&a[1]), a[0].d);
            return v_dbl(strtod(t, NULL));
        }
        if (n == 2) { if (a[0].k != V_DBL) return v_int(a[0].i); return v_int((long long)nearbyint(a[0].d)); }
        break;
    case BI_PARSEINT: case BI_INT:
        if (n == 0) return v_int(0);
        if (a[0].k == V_STR) {
            char *end;
            const char *s = a[0].o->s;
            while (isspace((unsigned char)*s) && vm->lang == LANG_PY) s++;
            long long v = strtoll(s, &end, n > 1 ? (int)as_int(&a[1]) : 10);
            while (isspace((unsigned char)*end) && vm->lang == LANG_PY) end++;
            if (end == s || *end) {
                if (vm->lang == LANG_PY) stop_with(vm, RUN_EXCEPTION, "ValueError: invalid literal for int() with base 10: '%s'", a[0].o->s);
                else stop_with(vm, RUN_EXCEPTION, "java.lang.NumberFormatException: For input string: \"%s\"", a[0].o->s);
                return none;
            }
            return v_int(v);
        }
        if (n == 1) { Val r; SemResult s = sem_un(LANG_PY, TY_DYN, U_TOINT, &a[0], &r, &vm->heap, err, sizeof(err)); if (s != SEM_OK) { stop_with(vm, s == SEM_TRAP ? RUN_EXCEPTION : RUN_UNSUPPORTED, "%s", err); return none; } if (a[0].k == V_DBL) r = v_int((long long)trunc(a[0].d)); return r; }
        break;
    case BI_FLOAT:
        if (n == 0) return v_dbl(0);
        if (a[0].k == V_STR) { char *end; double d = strtod(a[0].o->s, &end); if (end == a[0].o->s) { stop_with(vm, RUN_EXCEPTION, "ValueError: could not convert string to float: '%s'", a[0].o->s); return none; } return v_dbl(d); }
        if (v_is_num(&a[0])) return v_dbl(v_as_dbl(&a[0]));
        break;
    case BI_STR: case BI_VALUEOF: case BI_TOSTRING_INT:
        if (n == 0) return v_str(&vm->heap, "", 0);
        v_tostr(&b, &a[0], vm->lang, 0);
        return mk_str(vm, &b);
    case BI_BOOL: return v_bool(n ? v_truthy(&a[0]) : 0);
    case BI_SUM: {
        if (n < 1 || a[0].k != V_SEQ) break;
        Val acc = n > 1 ? a[1] : v_int(0);
        for (int i = 0; i < a[0].o->n && !vm->stop; i++) {
            Val r; SemResult s = sem_bin(vm->lang, TY_DYN, B_ADD, &acc, &a[0].o->items[i], &r, &vm->heap, err, sizeof(err));
            if (s != SEM_OK) { stop_with(vm, s == SEM_TRAP ? RUN_EXCEPTION : RUN_UNSUPPORTED, "%s", err); return none; }
            acc = r;
        }
        return acc;
    }
    case BI_SORTED: case BI_LIST: case BI_TUPLE: case BI_REVERSED: {
        if (n == 0) return v_seq(&vm->heap, id == BI_TUPLE ? SEQ_TUPLE : SEQ_LIST, 0, TY_DYN);
        int ok;
        Val r = to_list(vm, &a[0], id == BI_TUPLE ? SEQ_TUPLE : SEQ_LIST, &ok);
        if (!ok) return none;
        if (id == BI_SORTED) {
            sort_vals(vm, r.o->items, r.o->n);
            if (strstr(name, ":reverse") && n > 1 && v_truthy(&a[n - 1])) for (int i = 0, j = r.o->n - 1; i < j; i++, j--) { Val t = r.o->items[i]; r.o->items[i] = r.o->items[j]; r.o->items[j] = t; }
            else if (strstr(name, ":")) { stop_with(vm, RUN_UNSUPPORTED, "sorted() with a key function"); return none; }
        }
        if (id == BI_REVERSED) for (int i = 0, j = r.o->n - 1; i < j; i++, j--) { Val t = r.o->items[i]; r.o->items[i] = r.o->items[j]; r.o->items[j] = t; }
        return r;
    }
    case BI_ORD: if (n == 1 && a[0].k == V_STR && a[0].o->slen >= 1) { unsigned char c0 = (unsigned char)a[0].o->s[0]; return v_int(c0 < 0x80 ? c0 : ((c0 & 0x1F) << 6) | (a[0].o->s[1] & 0x3F)); } break;
    case BI_CHR: if (n == 1) { long long c = as_int(&a[0]); if (c < 0x80) { char ch = (char)c; return v_str(&vm->heap, &ch, 1); } Val t = v_int(c); t.k = V_CHAR; v_tostr(&b, &t, LANG_JAVA, 0); return mk_str(vm, &b); } break;
    case BI_RANGE: {
        long long st = 0, sp = 0, step = 1;
        if (n == 1) sp = as_int(&a[0]); else if (n >= 2) { st = as_int(&a[0]); sp = as_int(&a[1]); if (n == 3) step = as_int(&a[2]); }
        if (step == 0) { stop_with(vm, RUN_EXCEPTION, "ValueError: range() arg 3 must not be zero"); return none; }
        long long cnt = step > 0 ? (sp > st ? (sp - st + step - 1) / step : 0) : (st > sp ? (st - sp - step - 1) / -step : 0);
        if (cnt > 5000000) { stop_with(vm, RUN_LIMIT, "range() of %lld elements is too large to materialize", cnt); return none; }
        Val r = v_seq(&vm->heap, SEQ_LIST, (int)cnt, TY_DYN);
        for (long long i = 0; i < cnt; i++) r.o->items[i] = v_int(st + i * step);
        return r;
    }
    case BI_RANGE_CHECK:
        for (int i = 0; i < n; i++) if (a[i].k != V_INT && a[i].k != V_BOOL) { stop_with(vm, RUN_EXCEPTION, "TypeError: '%s' object cannot be interpreted as an integer", a[i].k == V_DBL ? "float" : a[i].k == V_STR ? "str" : "object"); return none; }
        if (n == 3 && a[2].i == 0) { stop_with(vm, RUN_EXCEPTION, "ValueError: range() arg 3 must not be zero"); return none; }
        return none;
    case BI_SLICE: {
        if (n != 4) break;
        Val s = a[0];
        long long len = s.k == V_STR ? (long long)s.o->slen : s.k == V_SEQ ? s.o->n : -1;
        if (len < 0) { stop_with(vm, RUN_EXCEPTION, "TypeError: object is not subscriptable"); return none; }
        long long step = a[3].k == V_NONE ? 1 : as_int(&a[3]);
        if (step == 0) { stop_with(vm, RUN_EXCEPTION, "ValueError: slice step cannot be zero"); return none; }
        long long lo, hi;
        if (a[1].k == V_NONE) lo = step > 0 ? 0 : len - 1; else { lo = as_int(&a[1]); if (lo < 0) lo += len; if (lo < 0) lo = step > 0 ? 0 : -1; if (lo > len) lo = step > 0 ? len : len - 1; if (step < 0 && lo >= len) lo = len - 1; }
        if (a[2].k == V_NONE) hi = step > 0 ? len : -1; else { hi = as_int(&a[2]); if (hi < 0) hi += len; if (hi < 0) hi = step > 0 ? 0 : -1; if (hi > len) hi = len; }
        if (s.k == V_STR) {
            for (long long i = lo; step > 0 ? i < hi : i > hi; i += step) bputc(&b, s.o->s[i]);
            return mk_str(vm, &b);
        }
        Val r = v_seq(&vm->heap, (SeqKind)s.o->seq, 0, TY_DYN);
        for (long long i = lo; step > 0 ? i < hi : i > hi; i += step) seq_push(vm, r.o, s.o->items[i]);
        return r;
    }
    case BI_FORMAT:
        if (n == 2 && a[1].k == V_STR) {
            if (!py_format(vm, &b, &a[0], a[1].o->s)) { free(b.s); stop_with(vm, RUN_UNSUPPORTED, "format spec '%s' is not modelled", a[1].o->s); return none; }
            return mk_str(vm, &b);
        }
        break;
    case BI_EXC: case BI_JEXC: {
        /* an exception object: kept as its message text */
        const char *cls = id == BI_JEXC ? name + 4 : name;
        if (id == BI_JEXC && !strchr(cls, '.')) bputs(&b, "java.lang.");
        bputs(&b, cls);
        if (n) { bputs(&b, ": "); v_tostr(&b, &a[0], vm->lang, 0); }
        return mk_str(vm, &b);
    }
    case BI_RAISE: case BI_THROW:
        if (n && a[0].k == V_STR) stop_with(vm, RUN_EXCEPTION, "%s", a[0].o->s);
        else stop_with(vm, RUN_EXCEPTION, "exception raised");
        return none;
    case BI_ASSERT:
        if (n && a[0].k == V_STR && a[0].o->slen) stop_with(vm, RUN_EXCEPTION, "AssertionError: %s", a[0].o->s);
        else stop_with(vm, RUN_EXCEPTION, vm->lang == LANG_JAVA ? "java.lang.AssertionError" : vm->lang == LANG_C ? "Assertion failed" : "AssertionError");
        return none;
    case BI_GCD: if (n == 2) { long long x = llabs(as_int(&a[0])), y = llabs(as_int(&a[1])); while (y) { long long t = x % y; x = y; y = t; } return v_int(x); } break;
    case BI_PI: return v_dbl(3.141592653589793);
    case BI_E: return v_dbl(2.718281828459045);
    case BI_DIVMOD: if (n == 2) {
        Val q, r;
        SemResult s1 = sem_bin(vm->lang, TY_DYN, B_FLOORDIV, &a[0], &a[1], &q, &vm->heap, err, sizeof(err));
        if (s1 != SEM_OK) { stop_with(vm, s1 == SEM_TRAP ? RUN_EXCEPTION : RUN_UNSUPPORTED, "%s", err); return none; }
        sem_bin(vm->lang, TY_DYN, B_MOD, &a[0], &a[1], &r, &vm->heap, err, sizeof(err));
        Val t = v_seq(&vm->heap, SEQ_TUPLE, 2, TY_DYN); t.o->items[0] = q; t.o->items[1] = r; return t;
    } break;
    case BI_ZIP: case BI_ENUMERATE: {
        if (n < 1) break;
        int ok;
        Val l0 = to_list(vm, &a[0], SEQ_LIST, &ok); if (!ok) return none;
        Val l1 = l0;
        if (id == BI_ZIP && n == 2) { l1 = to_list(vm, &a[1], SEQ_LIST, &ok); if (!ok) return none; }
        else if (id == BI_ZIP) break;
        int cnt = id == BI_ZIP && l1.o->n < l0.o->n ? l1.o->n : l0.o->n;
        long long base = id == BI_ENUMERATE && n > 1 ? as_int(&a[1]) : 0;
        Val r = v_seq(&vm->heap, SEQ_LIST, cnt, TY_DYN);
        for (int i = 0; i < cnt; i++) {
            Val t = v_seq(&vm->heap, SEQ_TUPLE, 2, TY_DYN);
            t.o->items[0] = id == BI_ZIP ? l0.o->items[i] : v_int(base + i);
            t.o->items[1] = id == BI_ZIP ? l1.o->items[i] : l0.o->items[i];
            r.o->items[i] = t;
        }
        return r;
    }
    case BI_ANY: case BI_ALL: {
        if (n != 1) break;
        int ok; Val l = to_list(vm, &a[0], SEQ_LIST, &ok); if (!ok) return none;
        for (int i = 0; i < l.o->n; i++) if (v_truthy(&l.o->items[i]) == (id == BI_ANY)) return v_bool(id == BI_ANY);
        return v_bool(id == BI_ALL);
    }
    /* Java strings (receiver first) */
    case BI_EQUALS: if (n == 2) return v_bool(a[1].k == V_STR && a[0].k == V_STR && a[0].o->slen == a[1].o->slen && !memcmp(a[0].o->s, a[1].o->s, a[0].o->slen)); break;
    case BI_COMPARETO: if (n == 2 && a[0].k == V_STR && a[1].k == V_STR) {
        size_t m = a[0].o->slen < a[1].o->slen ? a[0].o->slen : a[1].o->slen;
        for (size_t i = 0; i < m; i++) if (a[0].o->s[i] != a[1].o->s[i]) return v_int((unsigned char)a[0].o->s[i] - (unsigned char)a[1].o->s[i]);
        return v_int((long long)a[0].o->slen - (long long)a[1].o->slen);
    } break;
    case BI_ISEMPTY: if (n == 1 && a[0].k == V_STR) return v_bool(a[0].o->slen == 0); break;
    case BI_SUBSTRING: if ((n == 2 || n == 3) && a[0].k == V_STR) {
        long long lo = as_int(&a[1]), hi = n == 3 ? as_int(&a[2]) : (long long)a[0].o->slen;
        if (lo < 0 || hi > (long long)a[0].o->slen || lo > hi) { stop_with(vm, RUN_EXCEPTION, "java.lang.StringIndexOutOfBoundsException: begin %lld, end %lld, length %zu", lo, hi, a[0].o->slen); return none; }
        return v_str(&vm->heap, a[0].o->s + lo, (size_t)(hi - lo));
    } break;
    case BI_INDEXOF: case BI_FIND: if (n >= 2 && a[0].k == V_STR) {
        Buf nb2 = {0};
        v_tostr(&nb2, &a[1], vm->lang, 0);
        const char *hit = nb2.s ? strstr(a[0].o->s, nb2.s) : a[0].o->s;
        long long r = hit ? (long long)(hit - a[0].o->s) : -1;
        free(nb2.s);
        return v_int(r);
    } break;
    case BI_CONTAINS: if (n == 2 && a[0].k == V_STR && a[1].k == V_STR) return v_bool(strstr(a[0].o->s, a[1].o->s) != NULL); break;
    case BI_STARTSWITH_J: case BI_STARTSWITH: case BI_ENDSWITH:
        if (n == 2 && a[0].k == V_STR && a[1].k == V_STR) {
            size_t L0 = a[0].o->slen, L1 = a[1].o->slen;
            int r = L1 <= L0 && !memcmp(id == BI_ENDSWITH ? a[0].o->s + L0 - L1 : a[0].o->s, a[1].o->s, L1);
            return v_bool(r);
        }
        break;
    case BI_UPPER: case BI_LOWER: if (n == 1 && a[0].k == V_STR) {
        Val r = v_str(&vm->heap, a[0].o->s, a[0].o->slen);
        for (size_t i = 0; i < r.o->slen; i++) r.o->s[i] = (char)(id == BI_UPPER ? toupper((unsigned char)r.o->s[i]) : tolower((unsigned char)r.o->s[i]));
        return r;
    } break;
    case BI_ISDIGIT_PY: case BI_ISALPHA_PY: if (n == 1 && a[0].k == V_STR) {
        int ok = a[0].o->slen > 0;
        for (size_t i = 0; i < a[0].o->slen; i++) ok &= id == BI_ISDIGIT_PY ? isdigit((unsigned char)a[0].o->s[i]) != 0 : isalpha((unsigned char)a[0].o->s[i]) != 0;
        return v_bool(ok);
    } break;
    case BI_STRIP: case BI_LSTRIP: case BI_RSTRIP: if ((n == 1 || n == 2) && a[0].k == V_STR) {
        /* strip(chars): without chars, whitespace */
        const char *chars = n == 2 && a[1].k == V_STR ? a[1].o->s : NULL;
#define STRIPPABLE(c) (chars ? strchr(chars, (c)) != NULL && (c) != 0 : isspace((unsigned char)(c)))
        const char *s = a[0].o->s; size_t L0 = a[0].o->slen, i = 0;
        if (id != BI_RSTRIP) while (i < L0 && STRIPPABLE(s[i])) i++;
        if (id != BI_LSTRIP) while (L0 > i && STRIPPABLE(s[L0 - 1])) L0--;
#undef STRIPPABLE
        return v_str(&vm->heap, s + i, L0 - i);
    } break;
    case BI_SPLIT: if ((n == 1 || n == 2) && a[0].k == V_STR) {
        Val r = v_seq(&vm->heap, SEQ_LIST, 0, TY_DYN);
        const char *s = a[0].o->s, *e = s + a[0].o->slen;
        if (n == 1 || a[1].k == V_NONE) {
            while (s < e) {
                while (s < e && isspace((unsigned char)*s)) s++;
                const char *st = s;
                while (s < e && !isspace((unsigned char)*s)) s++;
                if (s > st) seq_push(vm, r.o, v_str(&vm->heap, st, (size_t)(s - st)));
            }
        } else if (a[1].k == V_STR && a[1].o->slen) {
            const char *sep = a[1].o->s; size_t sl = a[1].o->slen;
            for (;;) {
                const char *hit = NULL;
                for (const char *c = s; c + sl <= e; c++) if (!memcmp(c, sep, sl)) { hit = c; break; }
                if (!hit) { seq_push(vm, r.o, v_str(&vm->heap, s, (size_t)(e - s))); break; }
                seq_push(vm, r.o, v_str(&vm->heap, s, (size_t)(hit - s)));
                s = hit + sl;
            }
        } else break;
        return r;
    } break;
    case BI_JOIN: if (n == 2 && a[0].k == V_STR && a[1].k == V_SEQ) {
        for (int i = 0; i < a[1].o->n; i++) {
            if (a[1].o->items[i].k != V_STR) { stop_with(vm, RUN_EXCEPTION, "TypeError: sequence item %d: expected str instance", i); return none; }
            if (i) bput(&b, a[0].o->s, a[0].o->slen);
            bput(&b, a[1].o->items[i].o->s, a[1].o->items[i].o->slen);
        }
        return mk_str(vm, &b);
    } break;
    case BI_REPLACE: if (n == 3 && a[0].k == V_STR && a[1].k == V_STR && a[2].k == V_STR && a[1].o->slen) {
        const char *s = a[0].o->s, *e = s + a[0].o->slen;
        while (s < e) {
            if ((size_t)(e - s) >= a[1].o->slen && !memcmp(s, a[1].o->s, a[1].o->slen)) { bput(&b, a[2].o->s, a[2].o->slen); s += a[1].o->slen; }
            else bputc(&b, *s++);
        }
        return mk_str(vm, &b);
    } break;
    /* Python lists (receiver first) */
    case BI_APPEND: if (n == 2 && a[0].k == V_SEQ && a[0].o->seq == SEQ_LIST) { seq_push(vm, a[0].o, a[1]); return none; } break;
    case BI_POP: if ((n == 1 || n == 2) && a[0].k == V_SEQ && a[0].o->seq == SEQ_LIST) {
        if (!a[0].o->n) { stop_with(vm, RUN_EXCEPTION, "IndexError: pop from empty list"); return none; }
        int ok, i = n == 2 ? norm_index(vm, &a[0], as_int(&a[1]), a[0].o->n, &ok) : a[0].o->n - 1;
        if (n == 2 && !ok) return none;
        Val r = a[0].o->items[i];
        memmove(&a[0].o->items[i], &a[0].o->items[i + 1], sizeof(Val) * (size_t)(a[0].o->n - i - 1));
        a[0].o->n--;
        return r;
    } break;
    case BI_INSERT: if (n == 3 && a[0].k == V_SEQ && a[0].o->seq == SEQ_LIST) {
        long long i = as_int(&a[1]), L0 = a[0].o->n;
        if (i < 0) i += L0; if (i < 0) i = 0; if (i > L0) i = L0;
        seq_push(vm, a[0].o, v_none());
        memmove(&a[0].o->items[i + 1], &a[0].o->items[i], sizeof(Val) * (size_t)(L0 - i));
        a[0].o->items[i] = a[2];
        return none;
    } break;
    case BI_EXTEND: if (n == 2 && a[0].k == V_SEQ) { int ok; Val l = to_list(vm, &a[1], SEQ_LIST, &ok); if (!ok) return none; for (int i = 0; i < l.o->n; i++) seq_push(vm, a[0].o, l.o->items[i]); return none; } break;
    case BI_REVERSE: if (n == 1 && a[0].k == V_SEQ) { Obj *o = a[0].o; for (int i = 0, j = o->n - 1; i < j; i++, j--) { Val t = o->items[i]; o->items[i] = o->items[j]; o->items[j] = t; } return none; } break;
    case BI_SORT: if (a[0].k == V_SEQ && (n == 1 || (n == 2 && strstr(name, ":reverse")))) {
        sort_vals(vm, a[0].o->items, a[0].o->n);
        if (n == 2 && v_truthy(&a[1])) { Obj *o = a[0].o; for (int i = 0, j = o->n - 1; i < j; i++, j--) { Val t = o->items[i]; o->items[i] = o->items[j]; o->items[j] = t; } }
        return none;
    } break;
    case BI_INDEX: case BI_COUNT: if (n == 2 && a[0].k == V_SEQ) {
        int cnt = 0;
        for (int i = 0; i < a[0].o->n; i++) {
            Val r; sem_bin(vm->lang, TY_DYN, B_EQ, &a[0].o->items[i], &a[1], &r, &vm->heap, err, sizeof(err));
            if (v_truthy(&r)) { if (id == BI_INDEX) return v_int(i); cnt++; }
        }
        if (id == BI_INDEX) { stop_with(vm, RUN_EXCEPTION, "ValueError: value is not in list"); return none; }
        return v_int(cnt);
    } break;
    case BI_COPY: if (n == 1 && a[0].k == V_SEQ) { int ok; return to_list(vm, &a[0], (SeqKind)a[0].o->seq, &ok); } break;
    /* java.util.Arrays */
    case BI_ARR_TOSTRING: if (n == 1) {
        if (a[0].k == V_NONE) return v_str(&vm->heap, "null", 4);
        if (a[0].k != V_SEQ) break;
        bputc(&b, '[');
        for (int i = 0; i < a[0].o->n; i++) { if (i) bputs(&b, ", "); v_tostr(&b, &a[0].o->items[i], vm->lang, 0); }
        bputc(&b, ']');
        return mk_str(vm, &b);
    } break;
    case BI_ARR_SORT: if (n == 1 && a[0].k == V_SEQ) { sort_vals(vm, a[0].o->items, a[0].o->n); return none; } break;
    case BI_ARR_FILL: if (n == 2 && a[0].k == V_SEQ) { for (int i = 0; i < a[0].o->n; i++) a[0].o->items[i] = a[1]; return none; } break;
    default: break;
    }
    free(b.s);
    stop_with(vm, RUN_UNSUPPORTED, "the library function %s() (with %d argument%s) is not modelled by the interpreter", name, n, n == 1 ? "" : "s");
    return none;
}

/* ---- the interpreter loop ---- */

static Val exec_fn(VM *vm, int fi, Val *args, int nargs);

static Val *ref(VM *vm, Val *frame, XOp *o, Val *tmp) {
    if (o->kind == 1) return &frame[o->slot];
    if (o->kind == 2) return &vm->globals[o->slot];
    *tmp = o->k;
    return tmp;
}

static Val exec_fn(VM *vm, int fi, Val *args, int nargs) {
    XFunc *x = &vm->fx[fi];
    Val none = v_none();
    if (++vm->depth > vm->lim->max_depth) {
        stop_with(vm, RUN_LIMIT, "recursion deeper than %d calls (interpreter limit)", vm->lim->max_depth);
        vm->depth--;
        return none;
    }
    Val *frame = xcalloc((size_t)(x->nslots ? x->nslots : 1), sizeof(Val));
    for (int i = 0; i < x->nparams && i < nargs; i++) frame[x->param_slots[i]] = args[i];
    Val result = vm->lang == LANG_C ? (Val){0} : none;
    char err[300];
    int pc = 0;
    while (pc < x->n && !vm->stop) {
        XIns *in = &x->code[pc];
        if (++vm->r->steps > vm->lim->max_steps) { stop_with(vm, RUN_LIMIT, "step limit of %lld instructions reached (possible infinite loop)", vm->lim->max_steps); break; }
        if ((vm->r->steps & 0xFFF) == 0 && vm->heap.bytes > vm->lim->max_heap) { stop_with(vm, RUN_LIMIT, "memory limit reached"); break; }
        Val ta, tb, tc;
        switch (in->op) {
        case I_COPY: {
            Val v = *ref(vm, frame, &in->a, &ta);
            if (vm->lang == LANG_JAVA && in->dty == TY_CHAR && v.k == V_INT) v.k = V_CHAR;
            *ref(vm, frame, &in->d, &ta) = v;
            break;
        }
        case I_BIN: {
            Val r;
            SemResult s = sem_bin(vm->lang, (Ty)in->ty, (BinOp)in->sub, ref(vm, frame, &in->a, &ta), ref(vm, frame, &in->b, &tb), &r, &vm->heap, err, sizeof(err));
            if (s == SEM_TRAP) { stop_with(vm, RUN_EXCEPTION, "%s (line %d)", err, in->line); break; }
            if (s == SEM_UNSUP) { stop_with(vm, RUN_UNSUPPORTED, "%s (line %d)", err, in->line); break; }
            if (s == SEM_UB) {
                if (!vm->r->ub_count++) snprintf(vm->r->ub_first, sizeof(vm->r->ub_first), "line %d: %s", in->line, err);
                if (r.k == V_UNDEF || strstr(err, "division")) { stop_with(vm, RUN_EXCEPTION, "%s (line %d)", err, in->line); break; }
            }
            *ref(vm, frame, &in->d, &ta) = r;
            break;
        }
        case I_UN: {
            Val r;
            SemResult s = sem_un(vm->lang, (Ty)in->ty, (UnOp)in->sub, ref(vm, frame, &in->a, &ta), &r, &vm->heap, err, sizeof(err));
            if (s == SEM_TRAP) { stop_with(vm, RUN_EXCEPTION, "%s (line %d)", err, in->line); break; }
            if (s == SEM_UNSUP) { stop_with(vm, RUN_UNSUPPORTED, "%s (line %d)", err, in->line); break; }
            if (s == SEM_UB && !vm->r->ub_count++) snprintf(vm->r->ub_first, sizeof(vm->r->ub_first), "line %d: %s", in->line, err);
            *ref(vm, frame, &in->d, &ta) = r;
            break;
        }
        case I_LABEL: case I_NOP: break;
        case I_GOTO: pc = in->target; continue;
        case I_IF: case I_IFNOT: {
            Val *c = ref(vm, frame, &in->a, &ta);
            if (c->k == V_UNDEF) { stop_with(vm, RUN_EXCEPTION, "branch on an uninitialized value (line %d)", in->line); break; }
            if (v_truthy(c) == (in->op == I_IF)) { pc = in->target; continue; }
            break;
        }
        case I_PARAM: vpush(vm->args, *ref(vm, frame, &in->a, &ta)); break;
        case I_CALL: {
            int n = in->nargs;
            Val *argv = xmalloc(sizeof(Val) * (size_t)(n ? n : 1));
            if (n) memcpy(argv, &vm->args.v[vm->args.n - n], sizeof(Val) * (size_t)n);
            vm->args.n -= n;
            Val r;
            if (in->fn >= 0) r = exec_fn(vm, in->fn, argv, n);
            else if (in->fn == -1) {
                stop_with(vm, RUN_UNSUPPORTED, "call to %s() (line %d): external code the interpreter cannot run", in->name, in->line);
                r = none;
            } else r = call_builtin(vm, -1 - in->fn, in->name, argv, n);
            free(argv);
            if (in->d.kind) {
                if (vm->lang == LANG_JAVA && in->dty == TY_CHAR && r.k == V_INT) r.k = V_CHAR;
                *ref(vm, frame, &in->d, &ta) = r;
            }
            break;
        }
        case I_RET:
            if (in->a.kind) result = *ref(vm, frame, &in->a, &ta);
            pc = x->n;
            continue;
        case I_LOAD: {
            Val *base = ref(vm, frame, &in->a, &ta), *idx = ref(vm, frame, &in->b, &tb);
            if (base->k == V_NONE) { stop_with(vm, RUN_EXCEPTION, vm->lang == LANG_PY ? "TypeError: 'NoneType' object is not subscriptable (line %d)" : "java.lang.NullPointerException (line %d)", in->line); break; }
            if (idx->k != V_INT && idx->k != V_CHAR && idx->k != V_BOOL) { stop_with(vm, vm->lang == LANG_PY ? RUN_EXCEPTION : RUN_UNSUPPORTED, "TypeError: indices must be integers (line %d)", in->line); break; }
            int ok;
            if (base->k == V_STR) {
                if (vm->lang == LANG_C && idx->i == (long long)base->o->slen) {   /* the terminating '\0' */
                    *ref(vm, frame, &in->d, &tc) = v_int(0);
                    break;
                }
                int i = norm_index(vm, base, idx->i, (long long)base->o->slen, &ok);
                if (!ok) break;
                Val r = vm->lang == LANG_PY ? v_str(&vm->heap, base->o->s + i, 1) : v_int((unsigned char)base->o->s[i]);
                if (vm->lang == LANG_JAVA) r.k = V_CHAR;
                *ref(vm, frame, &in->d, &tc) = r;
            } else if (base->k == V_SEQ) {
                int i = norm_index(vm, base, idx->i, base->o->n, &ok);
                if (!ok) break;
                *ref(vm, frame, &in->d, &tc) = base->o->items[i];
            } else stop_with(vm, vm->lang == LANG_PY ? RUN_EXCEPTION : RUN_UNSUPPORTED, "value is not indexable (line %d)", in->line);
            break;
        }
        case I_STORE: {
            Val *base = ref(vm, frame, &in->a, &ta), *idx = ref(vm, frame, &in->b, &tb);
            Val v = *ref(vm, frame, &in->c, &tc);
            if (base->k == V_STR) { stop_with(vm, vm->lang == LANG_PY ? RUN_EXCEPTION : RUN_UNSUPPORTED, vm->lang == LANG_PY ? "TypeError: 'str' object does not support item assignment" : "writing into a string literal (line %d)", in->line); break; }
            if (base->k != V_SEQ) { stop_with(vm, RUN_EXCEPTION, vm->lang == LANG_JAVA ? "java.lang.NullPointerException (line %d)" : "assignment into a non-list value (line %d)", in->line); break; }
            if (base->o->seq == SEQ_TUPLE && vm->lang == LANG_PY && in->a.kind != 1) { stop_with(vm, RUN_EXCEPTION, "TypeError: 'tuple' object does not support item assignment"); break; }
            int ok;
            int i = norm_index(vm, base, idx->i, base->o->n, &ok);
            if (!ok) break;
            if (vm->lang == LANG_JAVA && base->o->elem == TY_CHAR && v.k == V_INT) v.k = V_CHAR;
            base->o->items[i] = v;
            break;
        }
        case I_NEWARR: {
            Val *na = ref(vm, frame, &in->a, &ta);
            long long n = as_int(na);
            Val *nb = in->b.kind ? ref(vm, frame, &in->b, &tb) : NULL;
            long long m = nb ? as_int(nb) : -1;
            if (n < 0 || m < -1 || (nb && m < 0)) { stop_with(vm, RUN_EXCEPTION, vm->lang == LANG_JAVA ? "java.lang.NegativeArraySizeException: %lld" : "negative array size %lld", n < 0 ? n : m); break; }
            if ((double)n * (double)(m > 0 ? m : 1) > 2e7) { stop_with(vm, RUN_LIMIT, "array of %lld elements exceeds the interpreter's memory limit", n * (m > 0 ? m : 1)); break; }
            int kind = (int)as_int(ref(vm, frame, &in->c, &tc));
            Val r = v_seq(&vm->heap, kind == 1 ? SEQ_LIST : kind == 2 ? SEQ_TUPLE : SEQ_ARRAY, (int)n, (Ty)(nb ? TY_ARR : in->ty));
            if (nb) for (long long i = 0; i < n; i++) r.o->items[i] = v_seq(&vm->heap, SEQ_ARRAY, (int)m, (Ty)in->ty);
            *ref(vm, frame, &in->d, &ta) = r;
            break;
        }
        case I_OPAQUE:
            stop_with(vm, RUN_UNSUPPORTED, "reached an untranslated construct at line %d: %s", in->line, in->name);
            break;
        default: break;
        }
        pc++;
    }
    free(frame);
    vm->depth--;
    return result;
}

typedef struct { VM *vm; } ThreadArg;

static void *run_thread(void *p) {
    VM *vm = ((ThreadArg *)p)->vm;
    for (int e = 0; e < vm->P->entry.n && !vm->stop; e++) {
        int fi = -1;
        for (int k = 0; k < vm->nf; k++) if (!strcmp(vm->P->f.v[k].name, vm->P->entry.v[e])) fi = k;
        if (fi < 0) continue;
        /* main(argc, argv) / main(String[] args) get an empty command line */
        Val args[2];
        int na = vm->fx[fi].nparams;
        if (na == 2) { args[0] = v_int(1); args[1] = v_seq(&vm->heap, SEQ_ARRAY, 1, TY_STR); args[1].o->items[0] = v_str(&vm->heap, "program", 7); }
        else if (na == 1) args[0] = v_seq(&vm->heap, SEQ_ARRAY, 0, TY_STR);
        Val r = exec_fn(vm, fi, args, na > 2 ? 0 : na);
        if (vm->lang == LANG_C && !strcmp(vm->P->entry.v[e], "main") && !vm->stop) vm->r->exit_code = (int)(r.k == V_UNDEF ? 0 : r.i);
    }
    return NULL;
}

void ir_run(IRProg *p, const RunLimits *lim, RunResult *r) {
    memset(r, 0, sizeof(*r));
    double t0 = now_ms();
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.P = p; vm.lang = p->lang; vm.r = r; vm.lim = lim;
    prepare(&vm);
    if (!p->entry.n) { r->status = RUN_UNSUPPORTED; snprintf(r->msg, sizeof(r->msg), "no entry point (no main function / module code)"); }
    else {
#ifdef __EMSCRIPTEN__
        /* browser build: no threads; the wasm stack is sized at link time (web/build_wasm.sh) */
        ThreadArg ta = { &vm };
        run_thread(&ta);
#else
        /* deep recursion needs a big stack: run on a thread with 512 MB reserved */
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, (size_t)512 << 20);
        ThreadArg ta = { &vm };
        if (pthread_create(&th, &at, run_thread, &ta) == 0) pthread_join(th, NULL);
        else run_thread(&ta);
        pthread_attr_destroy(&at);
#endif
    }
    for (int i = 0; i < vm.nf; i++) { free(vm.fx[i].code); free(vm.fx[i].param_slots); }
    free(vm.fx);
    free(vm.globals); free((void *)vm.gnames);
    heap_free(&vm.heap); heap_free(&vm.cheap);
    vfree(vm.args); vfree(vm.str_cache_obj); vfree(vm.str_cache_key);
    r->ms = now_ms() - t0;
}
