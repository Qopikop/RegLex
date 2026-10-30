#include "opt.h"
#include "value.h"
#include <stdint.h>
#include <math.h>

static const char *pass_names[] = {
    "Unreachable-Code Elimination", "Control-Flow Simplification", "Constant Propagation", "Constant Folding",
    "Algebraic Simplification", "Strength Reduction", "Copy Propagation", "Local Value Numbering (CSE)",
    "Global Common-Subexpression Elimination", "Loop-Invariant Code Motion", "Dead-Code Elimination" };
static const char *pass_keys[] = { "unreach", "cfg", "constprop", "fold", "algebra", "strength", "copyprop", "lvn", "gcse", "licm", "dce" };

static int bit_index(int bit) { int i = 0; while (bit > 1) { bit >>= 1; i++; } return i; }
const char *opt_pass_name(int bit) { return pass_names[bit_index(bit)]; }
const char *opt_pass_key(int bit) { return pass_keys[bit_index(bit)]; }

int opt_parse_passes(const char *list) {
    if (!list || !strcmp(list, "all")) return OPT_ALL;
    if (!strcmp(list, "none")) return 0;
    int m = 0;
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", list);
    for (char *save = NULL, *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save))
        for (int i = 0; i < 11; i++) if (!strcmp(t, pass_keys[i])) m |= 1 << i;
    return m;
}

/* ---------------------------------------------------------------------
 * CFG
 * ------------------------------------------------------------------- */

static int is_jump(int op) { return op == I_GOTO || op == I_IF || op == I_IFNOT; }

void cfg_build(const Func *f, CFG *g) {
    int n = f->code.n;
    memset(g, 0, sizeof(*g));
    g->block_of = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
    char *leader = xcalloc((size_t)n + 1, 1);
    if (n) leader[0] = 1;
    for (int i = 0; i < n; i++) {
        int op = f->code.v[i].op;
        if (op == I_LABEL) leader[i] = 1;
        if ((is_jump(op) || op == I_RET) && i + 1 < n) leader[i + 1] = 1;
    }
    int nb = 0;
    for (int i = 0; i < n; i++) nb += leader[i];
    g->b = xcalloc((size_t)(nb ? nb : 1), sizeof(BB));
    int b = -1;
    for (int i = 0; i < n; i++) {
        if (leader[i]) { b++; g->b[b].start = i; }
        g->block_of[i] = b;
        g->b[b].end = i + 1;
    }
    g->n = nb;
    /* label -> block */
    int maxlab = 0;
    for (int i = 0; i < n; i++) if (f->code.v[i].op == I_LABEL && f->code.v[i].lab > maxlab) maxlab = f->code.v[i].lab;
    int *lab = xmalloc(sizeof(int) * (size_t)(maxlab + 1));
    for (int i = 0; i <= maxlab; i++) lab[i] = -1;
    for (int i = 0; i < n; i++) if (f->code.v[i].op == I_LABEL) lab[f->code.v[i].lab] = g->block_of[i];
    for (int k = 0; k < nb; k++) {
        const Instr *last = &f->code.v[g->b[k].end - 1];
        int fall = k + 1 < nb && last->op != I_GOTO && last->op != I_RET;
        if (is_jump(last->op) && last->lab <= maxlab && lab[last->lab] >= 0) {
            int t = lab[last->lab];
            vpush(g->b[k].succ, t);
            vpush(g->b[t].pred, k);
        }
        if (fall) {
            int dup = 0;
            for (int s = 0; s < g->b[k].succ.n; s++) dup |= g->b[k].succ.v[s] == k + 1;
            if (!dup) { vpush(g->b[k].succ, k + 1); vpush(g->b[k + 1].pred, k); }
        }
    }
    free(lab);
    free(leader);
}

void cfg_free(CFG *g) {
    for (int i = 0; i < g->n; i++) { vfree(g->b[i].succ); vfree(g->b[i].pred); }
    free(g->b);
    free(g->block_of);
    memset(g, 0, sizeof(*g));
}

/* iterative dominators: dom[b] as a bitset over blocks */
typedef uint64_t Word;
#define WORDS(n) (((n) + 63) / 64)
static int  bs_get(const Word *s, int i) { return (int)((s[i >> 6] >> (i & 63)) & 1); }
static void bs_set(Word *s, int i) { s[i >> 6] |= (Word)1 << (i & 63); }
static void bs_clr(Word *s, int i) { s[i >> 6] &= ~((Word)1 << (i & 63)); }

static Word *dominators(const CFG *g) {
    int W = WORDS(g->n);
    Word *dom = xcalloc((size_t)(g->n * W ? g->n * W : 1), sizeof(Word));
    for (int b = 0; b < g->n; b++) for (int w = 0; w < W; w++) dom[b * W + w] = b == 0 ? 0 : ~(Word)0;
    if (g->n) bs_set(dom, 0);
    Word *tmp = xmalloc(sizeof(Word) * (size_t)(W ? W : 1));
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int b = 1; b < g->n; b++) {
            for (int w = 0; w < W; w++) tmp[w] = g->b[b].pred.n ? ~(Word)0 : 0;
            for (int p = 0; p < g->b[b].pred.n; p++) {
                int pb = g->b[b].pred.v[p];
                for (int w = 0; w < W; w++) tmp[w] &= dom[pb * W + w];
            }
            bs_set(tmp, b);
            if (memcmp(tmp, &dom[b * W], sizeof(Word) * (size_t)W)) { memcpy(&dom[b * W], tmp, sizeof(Word) * (size_t)W); changed = 1; }
        }
    }
    free(tmp);
    return dom;
}

void cfg_back_edges(const CFG *g, IntVec *out) {
    if (!g->n) return;
    Word *dom = dominators(g);
    int W = WORDS(g->n);
    for (int u = 0; u < g->n; u++)
        for (int s = 0; s < g->b[u].succ.n; s++) {
            int h = g->b[u].succ.v[s];
            if (bs_get(&dom[u * W], h)) { vpush(*out, u); vpush(*out, h); }
        }
    free(dom);
}

/* ---------------------------------------------------------------------
 * Pass context
 * ------------------------------------------------------------------- */

typedef struct {
    uint64_t *keys;
    int *vals, cap, n;
} VarMap;

typedef struct {
    IRProg *P;
    Func *f;
    OptReport *rep;
    Lang lang;
    VarMap vm;
    int nvars;
    char *is_global;          /* var index -> global */
    char *py_int;             /* var index -> proven Python int */
    int *ndefs;               /* var index -> number of definitions in the function */
    int has_opaque;
    Heap heap;
} Opt;

static uint64_t opnd_key(const Opnd *o) {
    if (o->k == OP_TEMP) return ((uint64_t)1 << 63) | (uint64_t)o->id;
    if (o->k == OP_VAR) return (uint64_t)(uintptr_t)o->s | o->g;
    return 0;
}

static int vm_find(VarMap *m, uint64_t key, int add, int *next) {
    if (!key) return -1;
    if (add && m->n * 2 >= m->cap) {
        int ncap = m->cap ? m->cap * 2 : 64;
        uint64_t *nk = xcalloc((size_t)ncap, sizeof(uint64_t));
        int *nv = xmalloc(sizeof(int) * (size_t)ncap);
        for (int i = 0; i < m->cap; i++) if (m->keys[i]) {
            size_t h = (size_t)(m->keys[i] * 0x9E3779B97F4A7C15ULL) & (size_t)(ncap - 1);
            while (nk[h]) h = (h + 1) & (size_t)(ncap - 1);
            nk[h] = m->keys[i]; nv[h] = m->vals[i];
        }
        free(m->keys); free(m->vals);
        m->keys = nk; m->vals = nv; m->cap = ncap;
    }
    if (!m->cap) return -1;
    size_t h = (size_t)(key * 0x9E3779B97F4A7C15ULL) & (size_t)(m->cap - 1);
    while (m->keys[h]) {
        if (m->keys[h] == key) return m->vals[h];
        h = (h + 1) & (size_t)(m->cap - 1);
    }
    if (!add) return -1;
    m->keys[h] = key;
    m->vals[h] = (*next)++;
    m->n++;
    return m->vals[h];
}

static int vidx(Opt *O, const Opnd *o) { return vm_find(&O->vm, opnd_key(o), 0, NULL); }

/* (Re)index every variable and temp of the function, count definitions, and
 * work out which Python values are proven integers. */
static void prepare(Opt *O) {
    free(O->vm.keys); free(O->vm.vals);
    memset(&O->vm, 0, sizeof(O->vm));
    O->nvars = 0;
    O->has_opaque = 0;
    Func *f = O->f;
    for (int i = 0; i < f->code.n; i++) {
        Instr *in = &f->code.v[i];
        Opnd *u[3];
        int nu = instr_uses(in, u);
        for (int k = 0; k < nu; k++) vm_find(&O->vm, opnd_key(u[k]), 1, &O->nvars);
        Opnd *d = instr_def(in);
        if (d) vm_find(&O->vm, opnd_key(d), 1, &O->nvars);
        if (in->op == I_OPAQUE) O->has_opaque = 1;
    }
    for (int i = 0; i < f->params.n; i++) { Opnd p = o_var(f->params.v[i], TY_UNKNOWN, 0); vm_find(&O->vm, opnd_key(&p), 1, &O->nvars); }
    free(O->is_global); free(O->py_int); free(O->ndefs);
    O->is_global = xcalloc((size_t)O->nvars + 1, 1);
    O->py_int = xcalloc((size_t)O->nvars + 1, 1);
    O->ndefs = xcalloc((size_t)O->nvars + 1, sizeof(int));
    for (int i = 0; i < f->code.n; i++) {
        Instr *in = &f->code.v[i];
        Opnd *u[3];
        int nu = instr_uses(in, u);
        for (int k = 0; k < nu; k++) { int v = vidx(O, u[k]); if (v >= 0 && u[k]->k == OP_VAR && u[k]->g) O->is_global[v] = 1; }
        Opnd *d = instr_def(in);
        if (d) { int v = vidx(O, d); O->ndefs[v]++; if (d->k == OP_VAR && d->g) O->is_global[v] = 1; }
    }
    if (O->lang != LANG_PY) return;
    /* greatest fixpoint: assume every local is an int, drop those with a non-int definition */
    for (int v = 0; v < O->nvars; v++) O->py_int[v] = !O->is_global[v] && O->ndefs[v] > 0;
    for (int i = 0; i < f->params.n; i++) { Opnd p = o_var(f->params.v[i], TY_UNKNOWN, 0); int v = vidx(O, &p); if (v >= 0) O->py_int[v] = 0; }
    int changed = 1;
#define INTOP(o) ((o).k == OP_INT || ((o).k == OP_BOOL) || (((o).k == OP_VAR || (o).k == OP_TEMP) && vidx(O, &(o)) >= 0 && O->py_int[vidx(O, &(o))]))
    while (changed) {
        changed = 0;
        for (int i = 0; i < f->code.n; i++) {
            Instr *in = &f->code.v[i];
            Opnd *d = instr_def(in);
            if (!d) continue;
            int v = vidx(O, d);
            if (!O->py_int[v]) continue;
            int ok = 0;
            if (in->op == I_COPY) ok = in->ty == TY_INT || INTOP(in->a);
            else if (in->op == I_BIN) {
                int sub = in->sub;
                ok = INTOP(in->a) && INTOP(in->b) &&
                     (sub == B_ADD || sub == B_SUB || sub == B_MUL || sub == B_AND || sub == B_OR || sub == B_XOR ||
                      ((sub == B_FLOORDIV || sub == B_MOD) && in->b.k == OP_INT && in->b.i != 0) ||
                      (sub == B_POW && in->b.k == OP_INT && in->b.i >= 0) ||
                      ((sub == B_SHL || sub == B_SHR) && in->b.k == OP_INT && in->b.i >= 0));
            } else if (in->op == I_UN) ok = (in->sub == U_LEN) || ((in->sub == U_NEG || in->sub == U_POS || in->sub == U_BNOT) && INTOP(in->a));
            if (!ok) { O->py_int[v] = 0; changed = 1; }
        }
    }
#undef INTOP
}

static void record(Opt *O, int pass, const Instr *before, const Instr *after, int block, const char *fmt, ...) {
    OptRecord r;
    memset(&r, 0, sizeof(r));
    r.pass = pass_names[bit_index(pass)];
    r.func = O->f->name;
    r.line = before ? before->line : after ? after->line : 0;
    r.block = block;
    r.id = before ? before->id : after ? after->id : -1;
    Buf b = {0};
    if (before) { instr_str(&b, before, O->lang); r.before = bdetach(&b); }
    if (after) { instr_str(&b, after, O->lang); r.after = bdetach(&b); }
    char why[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    r.why = xstrdup(why);
    O->rep->counts[bit_index(pass)]++;
    if (O->rep->rec.n < 5000) vpush(O->rep->rec, r);
    else { free(r.before); free(r.after); free(r.why); }
}

static int opnd_is_int(Opt *O, const Opnd *o, Ty ty) {
    if (O->lang == LANG_PY) {
        if (o->k == OP_INT) return 1;
        int v = vidx(O, o);
        return v >= 0 && O->py_int[v];
    }
    return ty_is_int(ty);
}

/* No side effects and cannot raise: safe to delete, hoist or execute early. */
static int pure_nontrapping(Opt *O, const Instr *in) {
    switch (in->op) {
    case I_COPY: return 1;
    case I_UN:
        if (O->lang == LANG_PY) return in->sub != U_LEN && opnd_is_int(O, &in->a, TY_DYN) && in->sub != U_TOSTR && in->sub != U_REPR;
        return in->sub != U_LEN && in->sub != U_TOSTR && in->sub != U_REPR && !(O->lang == LANG_C && (in->sub == U_TOINT || in->sub == U_TOLONG));
    case I_BIN: {
        int s = in->sub;
        if (s == B_IN || s == B_NOTIN) return 0;
        if (O->lang == LANG_PY && !(opnd_is_int(O, &in->a, TY_DYN) && opnd_is_int(O, &in->b, TY_DYN))) return 0;
        if (O->lang == LANG_JAVA && in->ty == TY_STR) return 0;          /* concat calls toString() */
        if (s == B_DIV || s == B_MOD || s == B_FLOORDIV) {
            if (!ty_is_int((Ty)in->ty) && O->lang != LANG_PY) return O->lang != LANG_PY;   /* float division never traps in C/Java */
            return in->b.k == OP_INT && in->b.i != 0 && in->b.i != -1;
        }
        if (s == B_POW) return in->b.k == OP_INT && in->b.i >= 0 && in->b.i < 64;
        if ((s == B_SHL || s == B_SHR) && O->lang != LANG_JAVA) return in->b.k == OP_INT && in->b.i >= 0 && in->b.i < 31;
        return 1;
    }
    default: return 0;
    }
}

/* ---------------------------------------------------------------------
 * Constant folding
 * ------------------------------------------------------------------- */

static int fold_instr(Opt *O, Instr *in, Opnd *out, char *why, size_t wl) {
    char err[200];
    Val a = opnd_val(&in->a, O->lang, &O->heap), b, r;
    SemResult s;
    if (in->op == I_BIN) {
        b = opnd_val(&in->b, O->lang, &O->heap);
        s = sem_bin(O->lang, (Ty)in->ty, (BinOp)in->sub, &a, &b, &r, &O->heap, err, sizeof(err));
    } else s = sem_un(O->lang, (Ty)in->ty, (UnOp)in->sub, &a, &r, &O->heap, err, sizeof(err));
    if (s != SEM_OK) {
        snprintf(why, wl, "%s", err[0] ? err : "not foldable");
        return s == SEM_UB ? -2 : -1;
    }
    if (r.k == V_STR && r.o->slen > 200) return -1;                /* keep code size bounded */
    if (!val_opnd(&r, (Ty)in->d.ty, out)) return -1;
    if (O->lang == LANG_PY && out->k == OP_INT) out->ty = TY_INT;
    return 0;
}

static int pass_fold(Opt *O, CFG *g) {
    int changed = 0;
    for (int i = 0; i < O->f->code.n; i++) {
        Instr *in = &O->f->code.v[i];
        if (!((in->op == I_BIN && o_const(&in->a) && o_const(&in->b)) || (in->op == I_UN && o_const(&in->a)))) continue;
        if (in->op == I_UN && in->sub == U_LEN && in->a.k != OP_STR) continue;
        Opnd c;
        char why[220];
        int r = fold_instr(O, in, &c, why, sizeof(why));
        if (r < 0) continue;      /* traps, undefined behaviour (-2) or not representable: leave it to run time */
        Instr before = *in;
        in->op = I_COPY; in->a = c; in->b = o_none(); in->sub = 0;
        in->ty = in->d.ty;
        record(O, OPT_FOLD, &before, in, g->block_of[i], "both operands are constants, so the result is computed at compile time");
        changed = 1;
    }
    return changed;
}

/* ---------------------------------------------------------------------
 * Global constant propagation (forward dataflow, lattice per variable)
 * ------------------------------------------------------------------- */

enum { LAT_UNDEF = 0, LAT_CONST = 1, LAT_NAC = 2 };
typedef struct { unsigned char k; Opnd c; } Lat;

static void lat_meet(Lat *a, const Lat *b) {
    if (b->k == LAT_UNDEF || a->k == LAT_NAC) return;
    if (a->k == LAT_UNDEF) { *a = *b; return; }
    if (b->k == LAT_NAC || !o_same(&a->c, &b->c) || a->c.ty != b->c.ty) a->k = LAT_NAC;
}

static void kill_globals(Opt *O, Lat *st) { for (int v = 0; v < O->nvars; v++) if (O->is_global[v]) st[v].k = LAT_NAC; }
static void kill_all(Opt *O, Lat *st) {
    /* an opaque operation may write any variable (pointers, frames...) -- only temps survive */
    for (int i = 0; i < O->vm.cap; i++) if (O->vm.keys[i] && !(O->vm.keys[i] >> 63)) st[O->vm.vals[i]].k = LAT_NAC;
}

static void const_transfer(Opt *O, Instr *in, Lat *st) {
    if (in->op == I_CALL) kill_globals(O, st);
    if (in->op == I_OPAQUE) kill_all(O, st);
    Opnd *d = instr_def(in);
    if (!d) return;
    int v = vidx(O, d);
    Lat r = { LAT_NAC, o_none() };
    if (in->op == I_COPY) {
        if (o_const(&in->a)) { r.k = LAT_CONST; r.c = in->a; }
        else { int a = vidx(O, &in->a); if (a >= 0 && st[a].k == LAT_CONST) r = st[a]; else if (a >= 0 && st[a].k == LAT_UNDEF) r.k = LAT_UNDEF; }
    } else if (in->op == I_BIN || in->op == I_UN) {
        Instr tmp = *in;
        int ok = 1;
        Opnd *ops[2] = { &tmp.a, &tmp.b };
        for (int k = 0; k < (in->op == I_BIN ? 2 : 1); k++) {
            if (o_const(ops[k])) continue;
            int a = vidx(O, ops[k]);
            if (a >= 0 && st[a].k == LAT_CONST) *ops[k] = st[a].c; else ok = 0;
        }
        if (ok && !(tmp.op == I_UN && tmp.sub == U_LEN && tmp.a.k != OP_STR)) {
            Opnd c; char why[200];
            if (fold_instr(O, &tmp, &c, why, sizeof(why)) == 0) { r.k = LAT_CONST; r.c = c; }
        }
    }
    if (in->op == I_COPY && in->ty == TY_INT && O->lang == LANG_PY && r.k == LAT_CONST && r.c.k != OP_INT) r.k = LAT_NAC;
    st[v] = r;
}

static int pass_constprop(Opt *O, CFG *g) {
    int nv = O->nvars, nb = g->n;
    if (!nb || !nv) return 0;
    Lat *in_st = xcalloc((size_t)nb * (size_t)nv, sizeof(Lat));
    Lat *out_st = xcalloc((size_t)nb * (size_t)nv, sizeof(Lat));
    Lat *cur = xmalloc(sizeof(Lat) * (size_t)nv);
    char *visited = xcalloc((size_t)nb, 1);
    /* entry: every variable unknown (parameters, globals, and uninitialized locals alike) */
    for (int v = 0; v < nv; v++) in_st[v].k = LAT_NAC;
    int changed = 1, iters = 0;
    while (changed && iters++ < 200) {
        changed = 0;
        for (int b = 0; b < nb; b++) {
            if (b > 0) {
                for (int v = 0; v < nv; v++) cur[v].k = LAT_UNDEF;
                for (int p = 0; p < g->b[b].pred.n; p++) {
                    int pb = g->b[b].pred.v[p];
                    if (!visited[pb]) continue;
                    for (int v = 0; v < nv; v++) lat_meet(&cur[v], &out_st[pb * nv + v]);
                }
                memcpy(&in_st[b * nv], cur, sizeof(Lat) * (size_t)nv);
            } else memcpy(cur, in_st, sizeof(Lat) * (size_t)nv);
            for (int i = g->b[b].start; i < g->b[b].end; i++) const_transfer(O, &O->f->code.v[i], cur);
            if (!visited[b] || memcmp(cur, &out_st[b * nv], sizeof(Lat) * (size_t)nv)) {
                /* compare lattice values field by field (padding-safe) */
                int diff = !visited[b];
                for (int v = 0; !diff && v < nv; v++) {
                    Lat *o = &out_st[b * nv + v];
                    diff = o->k != cur[v].k || (o->k == LAT_CONST && !o_same(&o->c, &cur[v].c));
                }
                if (diff) { memcpy(&out_st[b * nv], cur, sizeof(Lat) * (size_t)nv); changed = 1; }
                visited[b] = 1;
            }
        }
    }
    /* rewrite uses */
    int any = 0;
    for (int b = 0; b < nb; b++) {
        if (!visited[b]) continue;
        memcpy(cur, &in_st[b * nv], sizeof(Lat) * (size_t)nv);
        for (int i = g->b[b].start; i < g->b[b].end; i++) {
            Instr *in = &O->f->code.v[i];
            Opnd *u[3];
            int nu = instr_uses(in, u);
            Instr before = *in;
            int did = 0;
            const char *name = NULL;
            for (int k = 0; k < nu; k++) {
                if (in->op == I_LOAD && k == 0) continue;                 /* array base */
                if (in->op == I_STORE && k == 0) continue;
                if (in->op == I_NEWARR) continue;
                int v = vidx(O, u[k]);
                if (v < 0 || cur[v].k != LAT_CONST) continue;
                if (u[k]->k == OP_VAR) name = u[k]->s;
                Opnd c = cur[v].c;
                if (O->lang != LANG_PY && u[k]->ty != c.ty && ty_is_num((Ty)u[k]->ty)) {
                    if (c.k == OP_INT && ty_is_int((Ty)u[k]->ty)) c.ty = u[k]->ty;
                    else if (c.k == OP_FLT && (u[k]->ty == TY_DOUBLE || u[k]->ty == TY_FLOAT)) c.ty = u[k]->ty;
                    else continue;
                }
                *u[k] = c;
                did = 1;
            }
            if (did) {
                Buf nb2 = {0};
                if (name) bprintf(&nb2, "'%s'", name); else bputs(&nb2, "a temporary");
                record(O, OPT_CONSTPROP, &before, in, b,
                       "%s holds the same constant on every path reaching this instruction (reaching-constants dataflow)", nb2.s);
                free(nb2.s);
                any = 1;
            }
            const_transfer(O, in, cur);
        }
    }
    free(in_st); free(out_st); free(cur); free(visited);
    return any;
}

/* ---------------------------------------------------------------------
 * Algebraic simplification and strength reduction
 * ------------------------------------------------------------------- */

static int is_k(const Opnd *o, long long v) { return (o->k == OP_INT && o->i == v) || (o->k == OP_FLT && o->f == (double)v && !signbit(o->f)); }
static int is_val(const Opnd *o) { return o->k == OP_VAR || o->k == OP_TEMP; }

static int pass_algebra(Opt *O, CFG *g) {
    int changed = 0;
    for (int i = 0; i < O->f->code.n; i++) {
        Instr *in = &O->f->code.v[i];
        if (in->op != I_BIN) continue;
        if (O->lang == LANG_JAVA && in->ty == TY_STR) continue;
        int ints = opnd_is_int(O, &in->a, (Ty)in->ty) && opnd_is_int(O, &in->b, (Ty)in->ty) && (O->lang == LANG_PY || ty_is_int((Ty)in->ty));
        int flts = O->lang != LANG_PY && (in->ty == TY_DOUBLE || in->ty == TY_FLOAT);
        if (!ints && !flts) continue;
        Instr before = *in;
        Opnd res = o_none();
        const char *why = NULL;
        int s = in->sub;
        if (ints) {
            if ((s == B_ADD || s == B_OR || s == B_XOR) && is_k(&in->b, 0) && is_val(&in->a)) { res = in->a; why = "x + 0, x | 0 and x ^ 0 are x for integers"; }
            else if ((s == B_ADD || s == B_OR || s == B_XOR) && is_k(&in->a, 0) && is_val(&in->b)) { res = in->b; why = "0 + x is x for integers"; }
            else if ((s == B_SUB || s == B_SHL || s == B_SHR) && is_k(&in->b, 0) && is_val(&in->a)) { res = in->a; why = "x - 0 and x shifted by 0 are x"; }
            else if ((s == B_MUL || s == B_DIV || s == B_FLOORDIV) && is_k(&in->b, 1) && is_val(&in->a)) { res = in->a; why = "x * 1 and x / 1 are x"; }
            else if (s == B_MUL && is_k(&in->a, 1) && is_val(&in->b)) { res = in->b; why = "1 * x is x"; }
            else if ((s == B_MUL || s == B_AND) && (is_k(&in->b, 0) || is_k(&in->a, 0))) { res = o_int(0, O->lang == LANG_PY ? TY_INT : (Ty)in->ty); why = "x * 0 and x & 0 are 0 for an integer x (x is a plain value, so nothing is skipped)"; }
            else if ((s == B_SUB || s == B_XOR) && is_val(&in->a) && o_same(&in->a, &in->b)) { res = o_int(0, O->lang == LANG_PY ? TY_INT : (Ty)in->ty); why = "x - x and x ^ x are 0 for integers"; }
            else if (s == B_POW && is_k(&in->b, 1) && is_val(&in->a)) { res = in->a; why = "x ** 1 is x for integers"; }
        } else {
            /* floating point: only identities exact under IEEE 754 (x + 0.0 is NOT one: -0.0 + 0.0 = +0.0) */
            if ((s == B_MUL || s == B_DIV) && in->b.k == OP_FLT && in->b.f == 1.0 && is_val(&in->a) && in->a.ty == in->ty) { res = in->a; why = "x * 1.0 and x / 1.0 are exactly x in IEEE arithmetic"; }
            else if (s == B_SUB && in->b.k == OP_FLT && in->b.f == 0.0 && !signbit(in->b.f) && is_val(&in->a) && in->a.ty == in->ty) { res = in->a; why = "x - 0.0 is exactly x (even for -0.0)"; }
        }
        if (!why) continue;
        in->op = I_COPY; in->a = res; in->b = o_none(); in->sub = 0; in->ty = in->d.ty;
        record(O, OPT_ALGEBRA, &before, in, g->block_of[i], "%s", why);
        changed = 1;
    }
    return changed;
}

static int pass_strength(Opt *O, CFG *g) {
    int changed = 0;
    for (int i = 0; i < O->f->code.n; i++) {
        Instr *in = &O->f->code.v[i];
        if (in->op != I_BIN || in->sub != B_MUL) continue;
        Opnd x, k;
        if (in->b.k == OP_INT && is_val(&in->a)) { x = in->a; k = in->b; }
        else if (in->a.k == OP_INT && is_val(&in->b)) { x = in->b; k = in->a; }
        else continue;
        int ints = opnd_is_int(O, &x, (Ty)in->ty) && (O->lang == LANG_PY || ty_is_int((Ty)in->ty));
        if (!ints || k.i < 2) continue;
        Instr before = *in;
        if (k.i == 2) {
            in->sub = B_ADD; in->a = x; in->b = x;
            record(O, OPT_STRENGTH, &before, in, g->block_of[i], "multiplying by 2 becomes an addition (x + x), a cheaper operation");
            changed = 1;
        } else if ((k.i & (k.i - 1)) == 0 && (O->lang == LANG_JAVA || O->lang == LANG_PY || in->ty == TY_UINT)) {
            int sh = 0;
            while ((1LL << sh) < k.i) sh++;
            in->sub = B_SHL; in->a = x; in->b = o_int(sh, TY_INT);
            record(O, OPT_STRENGTH, &before, in, g->block_of[i],
                   "multiplying by %lld = 2^%d becomes a left shift; valid here because %s", k.i, sh,
                   O->lang == LANG_JAVA ? "Java integer arithmetic wraps exactly like a shift" :
                   O->lang == LANG_PY ? "x is a proven Python int (unbounded, so no overflow)" : "the operand is unsigned");
            changed = 1;
        }
    }
    return changed;
}

/* ---------------------------------------------------------------------
 * Copy propagation (available copies, forward dataflow)
 * ------------------------------------------------------------------- */

static int pass_copyprop(Opt *O, CFG *g) {
    Func *f = O->f;
    IntVec copies = {0};                         /* instruction index of each copy x = y */
    int *copy_of = xmalloc(sizeof(int) * (size_t)(f->code.n ? f->code.n : 1));
    for (int i = 0; i < f->code.n; i++) {
        Instr *in = &f->code.v[i];
        copy_of[i] = -1;
        if (in->op == I_COPY && is_val(&in->a) && !o_same(&in->a, &in->d) &&
            (O->lang == LANG_PY || in->a.ty == in->d.ty || (ty_is_int((Ty)in->a.ty) && ty_is_int((Ty)in->d.ty) && in->a.ty != TY_CHAR && in->d.ty != TY_CHAR))) {
            copy_of[i] = copies.n;
            vpush(copies, i);
        }
    }
    int nc = copies.n, nb = g->n;
    if (!nc || !nb) { vfree(copies); free(copy_of); return 0; }
    int W = WORDS(nc);
    Word *in_s = xcalloc((size_t)nb * W, sizeof(Word)), *out_s = xcalloc((size_t)nb * W, sizeof(Word));
    Word *cur = xmalloc(sizeof(Word) * (size_t)W);
    /* kill sets per variable: copies mentioning it */
    int nv = O->nvars;
    IntVec *mention = xcalloc((size_t)(nv ? nv : 1), sizeof(IntVec));
    for (int c = 0; c < nc; c++) {
        Instr *in = &f->code.v[copies.v[c]];
        vpush(mention[vidx(O, &in->d)], c);
        vpush(mention[vidx(O, &in->a)], c);
    }
#define KILLVAR(set, var) for (int q_ = 0; q_ < mention[var].n; q_++) bs_clr(set, mention[var].v[q_])
#define TRANSFER(set, i) do { \
        Instr *t_ = &f->code.v[i]; \
        if (t_->op == I_CALL || t_->op == I_OPAQUE) for (int v_ = 0; v_ < nv; v_++) if (O->is_global[v_] || t_->op == I_OPAQUE) { KILLVAR(set, v_); } \
        Opnd *d_ = instr_def(t_); \
        if (d_) { int v_ = vidx(O, d_); KILLVAR(set, v_); } \
        if (copy_of[i] >= 0) bs_set(set, copy_of[i]); \
    } while (0)
    for (int b = 1; b < nb; b++) for (int w = 0; w < W; w++) out_s[b * W + w] = ~(Word)0;
    int changed = 1, iters = 0;
    while (changed && iters++ < 200) {
        changed = 0;
        for (int b = 0; b < nb; b++) {
            for (int w = 0; w < W; w++) cur[w] = (b == 0 || !g->b[b].pred.n) ? 0 : ~(Word)0;
            for (int p = 0; p < g->b[b].pred.n; p++) for (int w = 0; w < W; w++) cur[w] &= out_s[g->b[b].pred.v[p] * W + w];
            if (b == 0) for (int w = 0; w < W; w++) cur[w] = 0;
            memcpy(&in_s[b * W], cur, sizeof(Word) * (size_t)W);
            for (int i = g->b[b].start; i < g->b[b].end; i++) TRANSFER(cur, i);
            if (memcmp(cur, &out_s[b * W], sizeof(Word) * (size_t)W)) { memcpy(&out_s[b * W], cur, sizeof(Word) * (size_t)W); changed = 1; }
        }
    }
    int any = 0;
    for (int b = 0; b < nb; b++) {
        memcpy(cur, &in_s[b * W], sizeof(Word) * (size_t)W);
        for (int i = g->b[b].start; i < g->b[b].end; i++) {
            Instr *in = &f->code.v[i];
            Opnd *u[3];
            int nu = instr_uses(in, u);
            Instr before = *in;
            int did = 0;
            for (int k = 0; k < nu; k++) {
                int v = vidx(O, u[k]);
                if (v < 0) continue;
                for (int q = 0; q < mention[v].n; q++) {
                    int c = mention[v].v[q];
                    Instr *cp = &f->code.v[copies.v[c]];
                    if (!bs_get(cur, c) || !o_same(&cp->d, u[k])) continue;
                    Opnd src = cp->a;
                    if (O->lang != LANG_PY) { src.ty = u[k]->ty; src.el = u[k]->el; src.rank = u[k]->rank; }
                    *u[k] = src;
                    did = 1;
                    break;
                }
            }
            if (did) {
                record(O, OPT_COPYPROP, &before, in, b, "a copy 'x = y' reaches this use on every path with neither x nor y changed since, so y is used directly");
                any = 1;
            }
            TRANSFER(cur, i);
        }
    }
#undef TRANSFER
#undef KILLVAR
    for (int v = 0; v < nv; v++) vfree(mention[v]);
    free(mention); free(in_s); free(out_s); free(cur); free(copy_of);
    vfree(copies);
    return any;
}

/* ---------------------------------------------------------------------
 * Local value numbering (CSE within a basic block)
 * ------------------------------------------------------------------- */

static int commutative(int s) { return s == B_ADD || s == B_MUL || s == B_AND || s == B_OR || s == B_XOR || s == B_EQ || s == B_NE; }

/* may this expression be shared between two evaluations? */
static int cse_candidate(Opt *O, const Instr *in) {
    if (in->op == I_BIN) {
        if (in->sub == B_IN || in->sub == B_NOTIN || in->sub == B_IS || in->sub == B_ISNOT) return 0;
        if (O->lang == LANG_JAVA && in->ty == TY_STR) return 0;      /* a new String object each time */
        if (O->lang == LANG_PY) return opnd_is_int(O, &in->a, TY_DYN) && opnd_is_int(O, &in->b, TY_DYN);
        return 1;
    }
    if (in->op == I_UN) {
        if (O->lang == LANG_PY) return in->sub != U_TOSTR && in->sub != U_REPR && in->sub != U_LEN && opnd_is_int(O, &in->a, TY_DYN);
        return in->sub != U_TOSTR && in->sub != U_REPR;
    }
    return 0;
}

static int same_expr(Opt *O, const Instr *x, const Instr *y) {
    (void)O;
    if (x->op != y->op || x->sub != y->sub || x->ty != y->ty) return 0;
    if (o_same(&x->a, &y->a) && (x->op == I_UN || o_same(&x->b, &y->b))) return 1;
    return x->op == I_BIN && commutative(x->sub) && o_same(&x->a, &y->b) && o_same(&x->b, &y->a);
}

static int pass_lvn(Opt *O, CFG *g) {
    int any = 0;
    Func *f = O->f;
    for (int b = 0; b < g->n; b++) {
        IntVec avail = {0};          /* instructions whose result is still held by their destination */
        for (int i = g->b[b].start; i < g->b[b].end; i++) {
            Instr *in = &f->code.v[i];
            if (cse_candidate(O, in) || (in->op == I_LOAD && O->lang != LANG_PY)) {
                for (int q = 0; q < avail.n; q++) {
                    Instr *e = &f->code.v[avail.v[q]];
                    if (e->op == I_LOAD ? (in->op == I_LOAD && o_same(&e->a, &in->a) && o_same(&e->b, &in->b) && e->ty == in->ty) : same_expr(O, e, in)) {
                        if (O->lang != LANG_PY && e->d.ty != in->d.ty) continue;
                        Instr before = *in;
                        in->op = I_COPY; in->a = e->d; in->b = o_none(); in->sub = 0; in->ty = in->d.ty;
                        record(O, OPT_LVN, &before, in, b, "the same expression was already computed into %s earlier in this basic block and its operands have not changed",
                               e->d.k == OP_TEMP ? "a temporary" : e->d.s);
                        any = 1;
                        break;
                    }
                }
            }
            /* invalidate: anything reading or held in the redefined variable; loads after stores/calls */
            Opnd *d = instr_def(in);
            int kill_loads = in->op == I_STORE || in->op == I_CALL || in->op == I_OPAQUE;
            int kill_globals = in->op == I_CALL || in->op == I_OPAQUE;
            int w = 0;
            for (int q = 0; q < avail.n; q++) {
                Instr *e = &f->code.v[avail.v[q]];
                int dead = (kill_loads && e->op == I_LOAD) || in->op == I_OPAQUE;
                if (d && (o_same(&e->a, d) || o_same(&e->b, d) || o_same(&e->d, d))) dead = 1;
                if (kill_globals && ((e->a.k == OP_VAR && e->a.g) || (e->b.k == OP_VAR && e->b.g) || (e->d.k == OP_VAR && e->d.g))) dead = 1;
                if (!dead) avail.v[w++] = avail.v[q];
            }
            avail.n = w;
            if ((cse_candidate(O, in) || (in->op == I_LOAD && O->lang != LANG_PY)) && !o_same(&in->d, &in->a) && !o_same(&in->d, &in->b))
                vpush(avail, i);
        }
        vfree(avail);
    }
    return any;
}

/* ---------------------------------------------------------------------
 * Global CSE: available expressions held in single-assignment temps
 * ------------------------------------------------------------------- */

static int pass_gcse(Opt *O, CFG *g) {
    Func *f = O->f;
    IntVec cand = {0};
    int *cidx = xmalloc(sizeof(int) * (size_t)(f->code.n ? f->code.n : 1));
    for (int i = 0; i < f->code.n; i++) {
        Instr *in = &f->code.v[i];
        cidx[i] = -1;
        if (!cse_candidate(O, in) || in->d.k != OP_TEMP || O->ndefs[vidx(O, &in->d)] != 1) continue;
        if (o_same(&in->d, &in->a) || o_same(&in->d, &in->b)) continue;
        cidx[i] = cand.n;
        vpush(cand, i);
    }
    int nc = cand.n, nb = g->n;
    if (nc < 2 || !nb) { vfree(cand); free(cidx); return 0; }
    int W = WORDS(nc);
    Word *in_s = xcalloc((size_t)nb * W, sizeof(Word)), *out_s = xcalloc((size_t)nb * W, sizeof(Word)), *cur = xmalloc(sizeof(Word) * (size_t)W);
    int nv = O->nvars;
    IntVec *uses_of = xcalloc((size_t)(nv ? nv : 1), sizeof(IntVec));
    for (int c = 0; c < nc; c++) {
        Instr *in = &f->code.v[cand.v[c]];
        int a = vidx(O, &in->a), b = in->op == I_BIN ? vidx(O, &in->b) : -1;
        if (a >= 0) vpush(uses_of[a], c);
        if (b >= 0 && b != a) vpush(uses_of[b], c);
    }
#define TR(set, i) do { \
        Instr *t_ = &f->code.v[i]; \
        if (t_->op == I_CALL || t_->op == I_OPAQUE) for (int v_ = 0; v_ < nv; v_++) if (O->is_global[v_] || t_->op == I_OPAQUE) for (int q_ = 0; q_ < uses_of[v_].n; q_++) bs_clr(set, uses_of[v_].v[q_]); \
        Opnd *d_ = instr_def(t_); \
        if (d_) { int v_ = vidx(O, d_); for (int q_ = 0; q_ < uses_of[v_].n; q_++) bs_clr(set, uses_of[v_].v[q_]); } \
        if (cidx[i] >= 0) bs_set(set, cidx[i]); \
    } while (0)
    for (int b = 1; b < nb; b++) for (int w = 0; w < W; w++) out_s[b * W + w] = ~(Word)0;
    int changed = 1, iters = 0;
    while (changed && iters++ < 200) {
        changed = 0;
        for (int b = 0; b < nb; b++) {
            for (int w = 0; w < W; w++) cur[w] = (b == 0 || !g->b[b].pred.n) ? 0 : ~(Word)0;
            for (int p = 0; p < g->b[b].pred.n && b; p++) for (int w = 0; w < W; w++) cur[w] &= out_s[g->b[b].pred.v[p] * W + w];
            memcpy(&in_s[b * W], cur, sizeof(Word) * (size_t)W);
            for (int i = g->b[b].start; i < g->b[b].end; i++) TR(cur, i);
            if (memcmp(cur, &out_s[b * W], sizeof(Word) * (size_t)W)) { memcpy(&out_s[b * W], cur, sizeof(Word) * (size_t)W); changed = 1; }
        }
    }
    int any = 0;
    for (int b = 0; b < nb; b++) {
        memcpy(cur, &in_s[b * W], sizeof(Word) * (size_t)W);
        for (int i = g->b[b].start; i < g->b[b].end; i++) {
            Instr *in = &f->code.v[i];
            if (cse_candidate(O, in)) {
                for (int c = 0; c < nc; c++) {
                    if (!bs_get(cur, c) || cand.v[c] == i) continue;
                    Instr *e = &f->code.v[cand.v[c]];
                    if (g->block_of[cand.v[c]] == b && cand.v[c] > i) continue;
                    if (!same_expr(O, e, in) || (O->lang != LANG_PY && e->d.ty != in->d.ty)) continue;
                    Instr before = *in;
                    in->op = I_COPY; in->a = e->d; in->b = o_none(); in->sub = 0; in->ty = in->d.ty;
                    record(O, OPT_GCSE, &before, in, b, "this expression is available on every path into block B%d (computed in block B%d, operands unchanged), so its earlier result is reused",
                           b, g->block_of[cand.v[c]]);
                    any = 1;
                    break;
                }
            }
            TR(cur, i);
        }
    }
#undef TR
    for (int v = 0; v < nv; v++) vfree(uses_of[v]);
    free(uses_of); free(in_s); free(out_s); free(cur); free(cidx);
    vfree(cand);
    return any;
}

/* ---------------------------------------------------------------------
 * Liveness and dead-code elimination
 * ------------------------------------------------------------------- */

static int pass_dce(Opt *O, CFG *g) {
    Func *f = O->f;
    int nv = O->nvars, nb = g->n;
    if (!nb) return 0;
    int W = WORDS(nv ? nv : 1);
    Word *in_s = xcalloc((size_t)nb * W, sizeof(Word)), *out_s = xcalloc((size_t)nb * W, sizeof(Word)), *cur = xmalloc(sizeof(Word) * (size_t)W);
    Word *globals = xcalloc((size_t)W, sizeof(Word)), *allvars = xcalloc((size_t)W, sizeof(Word));
    for (int v = 0; v < nv; v++) { if (O->is_global[v]) bs_set(globals, v); }
    for (int i = 0; i < O->vm.cap; i++) if (O->vm.keys[i] && !(O->vm.keys[i] >> 63)) bs_set(allvars, O->vm.vals[i]);
#define LIVE_STEP(set, in) do { \
        Opnd *d_ = instr_def(in); \
        if (d_) bs_clr(set, vidx(O, d_)); \
        if ((in)->op == I_CALL || (in)->op == I_RET) for (int w_ = 0; w_ < W; w_++) set[w_] |= globals[w_]; \
        if ((in)->op == I_OPAQUE) for (int w_ = 0; w_ < W; w_++) set[w_] |= allvars[w_]; \
        Opnd *u_[3]; int nu_ = instr_uses(in, u_); \
        for (int k_ = 0; k_ < nu_; k_++) { int v_ = vidx(O, u_[k_]); if (v_ >= 0) bs_set(set, v_); } \
    } while (0)
    int changed = 1, iters = 0;
    while (changed && iters++ < 300) {
        changed = 0;
        for (int b = nb - 1; b >= 0; b--) {
            memset(cur, 0, sizeof(Word) * (size_t)W);
            for (int s = 0; s < g->b[b].succ.n; s++) for (int w = 0; w < W; w++) cur[w] |= in_s[g->b[b].succ.v[s] * W + w];
            if (!g->b[b].succ.n) for (int w = 0; w < W; w++) cur[w] |= globals[w];   /* globals outlive the function */
            memcpy(&out_s[b * W], cur, sizeof(Word) * (size_t)W);
            for (int i = g->b[b].end - 1; i >= g->b[b].start; i--) LIVE_STEP(cur, &f->code.v[i]);
            if (memcmp(cur, &in_s[b * W], sizeof(Word) * (size_t)W)) { memcpy(&in_s[b * W], cur, sizeof(Word) * (size_t)W); changed = 1; }
        }
    }
    int any = 0;
    for (int b = 0; b < nb; b++) {
        memcpy(cur, &out_s[b * W], sizeof(Word) * (size_t)W);
        for (int i = g->b[b].end - 1; i >= g->b[b].start; i--) {
            Instr *in = &f->code.v[i];
            Opnd *d = instr_def(in);
            int removable = d && (pure_nontrapping(O, in) || (in->op == I_LOAD && O->lang == LANG_C) ||
                                  (in->op == I_NEWARR && in->a.k == OP_INT && in->a.i >= 0 && !in->b.k));
            if (in->op == I_COPY && o_same(&in->d, &in->a)) {
                Instr before = *in;
                in->op = I_NOP;
                record(O, OPT_DCE, &before, NULL, b, "copying a variable to itself has no effect");
                any = 1;
                continue;
            }
            if (removable && !bs_get(cur, vidx(O, d))) {
                Instr before = *in;
                in->op = I_NOP;
                record(O, OPT_DCE, &before, NULL, b, "%s is never read afterwards (liveness analysis) and computing it has no side effects",
                       d->k == OP_TEMP ? "the temporary" : d->s);
                any = 1;
                continue;
            }
            LIVE_STEP(cur, in);
        }
    }
#undef LIVE_STEP
    free(in_s); free(out_s); free(cur); free(globals); free(allvars);
    return any;
}

/* ---------------------------------------------------------------------
 * Unreachable code and control-flow simplification
 * ------------------------------------------------------------------- */

static int pass_unreach(Opt *O, CFG *g) {
    if (!g->n) return 0;
    char *seen = xcalloc((size_t)g->n, 1);
    IntVec st = {0};
    vpush(st, 0);
    seen[0] = 1;
    while (st.n) {
        int b = st.v[--st.n];
        for (int s = 0; s < g->b[b].succ.n; s++) if (!seen[g->b[b].succ.v[s]]) { seen[g->b[b].succ.v[s]] = 1; vpush(st, g->b[b].succ.v[s]); }
    }
    vfree(st);
    int any = 0;
    for (int b = 0; b < g->n; b++) {
        if (seen[b]) continue;
        for (int i = g->b[b].start; i < g->b[b].end; i++) {
            Instr *in = &O->f->code.v[i];
            if (in->op == I_NOP) continue;
            if (in->op == I_LABEL) { in->op = I_NOP; continue; }
            Instr before = *in;
            in->op = I_NOP;
            record(O, OPT_UNREACH, &before, NULL, b, "block B%d cannot be reached from the function entry (no path in the control-flow graph)", b);
            any = 1;
        }
    }
    free(seen);
    return any;
}

static int next_real(Func *f, int i) {           /* first non-label, non-nop at or after i */
    while (i < f->code.n && (f->code.v[i].op == I_LABEL || f->code.v[i].op == I_NOP)) i++;
    return i;
}

static int label_index(Func *f, int lab) {
    for (int i = 0; i < f->code.n; i++) if (f->code.v[i].op == I_LABEL && f->code.v[i].lab == lab) return i;
    return -1;
}

static int pass_cfg(Opt *O, CFG *g) {
    Func *f = O->f;
    int any = 0;
    for (int i = 0; i < f->code.n; i++) {
        Instr *in = &f->code.v[i];
        /* branch on a constant */
        if ((in->op == I_IF || in->op == I_IFNOT) && o_const(&in->a)) {
            Val v = opnd_val(&in->a, O->lang, &O->heap);
            int taken = v_truthy(&v) == (in->op == I_IF);
            Instr before = *in;
            if (taken) in->op = I_GOTO; else in->op = I_NOP;
            record(O, OPT_CFG, &before, taken ? in : NULL, g->block_of[i],
                   taken ? "the condition is a constant that is always %s, so the branch is always taken" : "the condition is a constant that is always %s, so the branch is never taken",
                   v_truthy(&v) ? "true" : "false");
            any = 1;
            continue;
        }
        if (!is_jump(in->op)) continue;
        /* jump threading: a jump to a 'goto M' goes straight to M */
        for (int hops = 0; hops < 8; hops++) {
            int li = label_index(f, in->lab);
            if (li < 0) break;
            int t = next_real(f, li);
            if (t >= f->code.n || f->code.v[t].op != I_GOTO || f->code.v[t].lab == in->lab) break;
            Instr before = *in;
            in->lab = f->code.v[t].lab;
            record(O, OPT_CFG, &before, in, g->block_of[i], "the target label is followed directly by 'goto L%d', so the jump goes there in one step (jump threading)", in->lab);
            any = 1;
        }
        /* if c goto L1; goto L2; L1:  ->  ifFalse c goto L2; L1: */
        if ((in->op == I_IF || in->op == I_IFNOT) && i + 1 < f->code.n) {
            int j = i + 1;
            while (j < f->code.n && f->code.v[j].op == I_NOP) j++;
            if (j < f->code.n && f->code.v[j].op == I_GOTO) {
                int k = j + 1;
                int falls = 0;
                while (k < f->code.n && (f->code.v[k].op == I_LABEL || f->code.v[k].op == I_NOP)) { if (f->code.v[k].op == I_LABEL && f->code.v[k].lab == in->lab) falls = 1; k++; }
                if (falls) {
                    Instr before = *in;
                    in->op = in->op == I_IF ? I_IFNOT : I_IF;
                    in->lab = f->code.v[j].lab;
                    Instr gb = f->code.v[j];
                    f->code.v[j].op = I_NOP;
                    record(O, OPT_CFG, &before, in, g->block_of[i], "a conditional jump over an unconditional one is replaced by the inverted condition");
                    record(O, OPT_CFG, &gb, NULL, g->block_of[j], "absorbed into the inverted conditional jump above");
                    any = 1;
                }
            }
        }
        /* goto the very next instruction */
        if (in->op == I_GOTO || in->op == I_IF || in->op == I_IFNOT) {
            int k = i + 1, falls = 0;
            while (k < f->code.n && (f->code.v[k].op == I_LABEL || f->code.v[k].op == I_NOP)) { if (f->code.v[k].op == I_LABEL && f->code.v[k].lab == in->lab) falls = 1; k++; }
            if (falls && (in->op == I_GOTO || !o_const(&in->a))) {
                if (in->op != I_GOTO && O->lang == LANG_PY) continue;    /* evaluating the condition still matters only for side effects: none here */
                Instr before = *in;
                in->op = I_NOP;
                record(O, OPT_CFG, &before, NULL, g->block_of[i], "the jump targets the very next instruction, so it does nothing");
                any = 1;
            }
        }
    }
    /* labels no jump refers to are removed */
    int maxlab = 0;
    for (int i = 0; i < f->code.n; i++) if (f->code.v[i].lab > maxlab) maxlab = f->code.v[i].lab;
    char *used = xcalloc((size_t)maxlab + 1, 1);
    for (int i = 0; i < f->code.n; i++) if (is_jump(f->code.v[i].op)) used[f->code.v[i].lab] = 1;
    for (int i = 0; i < f->code.n; i++) if (f->code.v[i].op == I_LABEL && !used[f->code.v[i].lab]) f->code.v[i].op = I_NOP;
    free(used);
    return any;
}

/* ---------------------------------------------------------------------
 * Loop-invariant code motion
 * ------------------------------------------------------------------- */

static int pass_licm(Opt *O, CFG *g) {
    Func *f = O->f;
    IntVec be = {0};
    cfg_back_edges(g, &be);
    int any = 0;
    for (int e = 0; e < be.n && !any; e += 2) {
        int tail = be.v[e], head = be.v[e + 1];
        /* natural loop: head plus everything that reaches tail without passing head */
        char *in_loop = xcalloc((size_t)g->n, 1);
        in_loop[head] = 1;
        IntVec st = {0};
        if (!in_loop[tail]) { in_loop[tail] = 1; vpush(st, tail); }
        while (st.n) {
            int b = st.v[--st.n];
            for (int p = 0; p < g->b[b].pred.n; p++) { int q = g->b[b].pred.v[p]; if (!in_loop[q]) { in_loop[q] = 1; vpush(st, q); } }
        }
        vfree(st);
        /* the only way in must be falling through from the block just before the header */
        int outside = 0, pre = -1;
        for (int p = 0; p < g->b[head].pred.n; p++) { int q = g->b[head].pred.v[p]; if (!in_loop[q]) { outside++; pre = q; } }
        int insert_at = g->b[head].start;
        if (outside != 1 || pre != head - 1) { free(in_loop); continue; }
        Instr *pl = &f->code.v[g->b[pre].end - 1];
        if (is_jump(pl->op) && !(pl->op == I_GOTO && pl->lab == f->code.v[g->b[head].start].lab)) { free(in_loop); continue; }
        if (pl->op == I_GOTO) insert_at = g->b[pre].end - 1;
        /* variables defined inside the loop */
        char *def_in = xcalloc((size_t)O->nvars + 1, 1);
        int loop_opaque = 0, loop_calls = 0;
        for (int b = 0; b < g->n; b++) if (in_loop[b])
            for (int i = g->b[b].start; i < g->b[b].end; i++) {
                Opnd *d = instr_def(&f->code.v[i]);
                if (d) def_in[vidx(O, d)] = 1;
                if (f->code.v[i].op == I_OPAQUE) loop_opaque = 1;
                if (f->code.v[i].op == I_CALL) loop_calls = 1;
            }
        if (loop_opaque) { free(in_loop); free(def_in); continue; }
        IntVec hoist = {0};
        int progress = 1;
        while (progress) {
            progress = 0;
            for (int b = 0; b < g->n; b++) if (in_loop[b])
                for (int i = g->b[b].start; i < g->b[b].end; i++) {
                    Instr *in = &f->code.v[i];
                    if ((in->op != I_BIN && in->op != I_UN) || in->d.k != OP_TEMP) continue;
                    int dv = vidx(O, &in->d);
                    if (O->ndefs[dv] != 1 || !def_in[dv] || !pure_nontrapping(O, in)) continue;
                    int inv = 1;
                    Opnd *u[3];
                    int nu = instr_uses(in, u);
                    for (int k = 0; k < nu; k++) {
                        int v = vidx(O, u[k]);
                        if (v >= 0 && (def_in[v] || (loop_calls && O->is_global[v]))) inv = 0;
                    }
                    if (!inv) continue;
                    vpush(hoist, i);
                    def_in[dv] = 0;              /* now defined outside: its users may become invariant */
                    progress = 1;
                }
        }
        if (hoist.n) {
            /* move the instructions, in order, to just before the loop header */
            Code nc = {0};
            char *moved = xcalloc((size_t)f->code.n, 1);
            for (int h = 0; h < hoist.n; h++) moved[hoist.v[h]] = 1;
            for (int i = 0; i < f->code.n; i++) {
                if (i == insert_at)
                    for (int h = 0; h < hoist.n; h++) {
                        Instr *m = &f->code.v[hoist.v[h]];
                        record(O, OPT_LICM, m, m, g->block_of[hoist.v[h]],
                               "its operands do not change inside the loop headed by block B%d, so it is computed once before the loop instead of on every iteration", head);
                        vpush(nc, *m);
                    }
                if (!moved[i]) vpush(nc, f->code.v[i]);
            }
            vfree(f->code);
            f->code = nc;
            free(moved);
            any = 1;
        }
        vfree(hoist);
        free(in_loop); free(def_in);
    }
    vfree(be);
    return any;
}

/* ---------------------------------------------------------------------
 * Driver
 * ------------------------------------------------------------------- */

static void compact(Func *f) {
    int w = 0;
    for (int i = 0; i < f->code.n; i++) if (f->code.v[i].op != I_NOP) f->code.v[w++] = f->code.v[i];
    f->code.n = w;
}

typedef int (*PassFn)(Opt *, CFG *);

void optimize(IRProg *p, int passes, OptReport *rep) {
    double t0 = now_ms();
    static const struct { int bit; PassFn fn; } order[] = {
        { OPT_UNREACH, pass_unreach }, { OPT_CFG, pass_cfg }, { OPT_CONSTPROP, pass_constprop }, { OPT_FOLD, pass_fold },
        { OPT_ALGEBRA, pass_algebra }, { OPT_STRENGTH, pass_strength }, { OPT_COPYPROP, pass_copyprop },
        { OPT_LVN, pass_lvn }, { OPT_GCSE, pass_gcse }, { OPT_LICM, pass_licm }, { OPT_DCE, pass_dce },
    };
    Opt O;
    memset(&O, 0, sizeof(O));
    O.P = p; O.rep = rep; O.lang = p->lang;
    int max_rounds = 0;
    for (int fi = 0; fi < p->f.n; fi++) {
        O.f = &p->f.v[fi];
        int round = 0, changed = 1;
        while (changed && round < 10) {
            changed = 0;
            round++;
            for (size_t k = 0; k < sizeof(order) / sizeof(order[0]); k++) {
                if (!(passes & order[k].bit)) continue;
                prepare(&O);
                CFG g;
                cfg_build(O.f, &g);
                if (order[k].fn(&O, &g)) changed = 1;
                cfg_free(&g);
                compact(O.f);
            }
        }
        if (round > max_rounds) max_rounds = round;
    }
    rep->rounds = max_rounds;
    free(O.vm.keys); free(O.vm.vals); free(O.is_global); free(O.py_int); free(O.ndefs);
    heap_free(&O.heap);
    rep->ms = now_ms() - t0;
}
