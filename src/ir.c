#include "ir.h"
#include <math.h>

const char *ty_name(Ty t) {
    static const char *n[] = { "?", "void", "bool", "char", "int", "unsigned", "long", "float", "double",
                               "str", "array", "object", "dyn", "null" };
    return t < TY__N ? n[t] : "?";
}
const char *binop_sym(BinOp b) {
    static const char *n[] = { "+", "-", "*", "/", "%", "//", "**", "&", "|", "^", "<<", ">>", ">>>",
                               "==", "!=", "<", "<=", ">", ">=", "in", "not in", "is", "is not" };
    return n[b];
}
const char *unop_sym(UnOp u) {
    static const char *n[] = { "-", "!", "~", "+", "(int)", "(long)", "(double)", "(float)", "(char)",
                               "(bool)", "str", "repr", "len" };
    return n[u];
}
int ty_is_int(Ty t) { return t == TY_BOOL || t == TY_CHAR || t == TY_INT || t == TY_UINT || t == TY_LONG; }
int ty_is_num(Ty t) { return ty_is_int(t) || t == TY_FLOAT || t == TY_DOUBLE; }

Opnd o_none(void) { Opnd o; memset(&o, 0, sizeof(o)); return o; }
Opnd o_int(long long v, Ty ty) { Opnd o = o_none(); o.k = OP_INT; o.i = v; o.ty = (unsigned char)ty; return o; }
Opnd o_flt(double v, Ty ty) { Opnd o = o_none(); o.k = OP_FLT; o.f = v; o.ty = (unsigned char)ty; return o; }
Opnd o_str(const char *s) { Opnd o = o_none(); o.k = OP_STR; o.s = s; o.ty = TY_STR; return o; }
Opnd o_bool(int v) { Opnd o = o_none(); o.k = OP_BOOL; o.i = v != 0; o.ty = TY_BOOL; return o; }
Opnd o_null(void) { Opnd o = o_none(); o.k = OP_NULL; o.ty = TY_NULL; return o; }
Opnd o_var(const char *name, Ty ty, int global) { Opnd o = o_none(); o.k = OP_VAR; o.s = name; o.ty = (unsigned char)ty; o.g = (unsigned char)global; return o; }
int o_const(const Opnd *o) { return o->k >= OP_INT; }

int o_same(const Opnd *a, const Opnd *b) {
    if (a->k != b->k) return 0;
    switch (a->k) {
    case OP_VAR: return a->s == b->s && a->g == b->g;
    case OP_TEMP: return a->id == b->id;
    case OP_INT: case OP_BOOL: return a->i == b->i && a->ty == b->ty;
    case OP_FLT: return memcmp(&a->f, &b->f, sizeof(double)) == 0 && a->ty == b->ty;
    case OP_STR: return a->s == b->s;
    case OP_NULL: return 1;
    default: return 0;
    }
}

/* ---- text ---- */

static void flt_str(Buf *b, double v) {
    if (isnan(v)) { bputs(b, "nan"); return; }
    if (isinf(v)) { bputs(b, v > 0 ? "inf" : "-inf"); return; }
    char tmp[40];
    for (int p = 1; p <= 17; p++) { snprintf(tmp, sizeof(tmp), "%.*g", p, v); if (strtod(tmp, NULL) == v) break; }
    bputs(b, tmp);
    if (!strpbrk(tmp, ".en")) bputs(b, ".0");
}

void opnd_str(Buf *b, const Opnd *o, Lang lang) {
    switch (o->k) {
    case OP_VAR: bputs(b, o->s); break;
    case OP_TEMP: bprintf(b, "t%d", o->id); break;
    case OP_INT:
        if (o->ty == TY_CHAR && o->i >= 32 && o->i < 127 && o->i != '\'' && o->i != '\\') bprintf(b, "'%c'", (int)o->i);
        else bprintf(b, "%lld", o->i);
        if (o->ty == TY_LONG && lang != LANG_PY) bputc(b, 'L');
        break;
    case OP_FLT: flt_str(b, o->f); if (o->ty == TY_FLOAT) bputc(b, 'f'); break;
    case OP_STR: {
        size_t n = strlen(o->s);
        bjson_str(b, o->s, n > 48 ? 48 : n);
        if (n > 48) { b->n--; bputs(b, "\xe2\x80\xa6\""); }
        break;
    }
    case OP_BOOL: bputs(b, lang == LANG_PY ? (o->i ? "True" : "False") : (o->i ? "true" : "false")); break;
    case OP_NULL: bputs(b, lang == LANG_PY ? "None" : lang == LANG_C ? "NULL" : "null"); break;
    default: bputs(b, "_");
    }
}

void instr_str(Buf *b, const Instr *in, Lang lang) {
    switch (in->op) {
    case I_COPY:  opnd_str(b, &in->d, lang); bputs(b, " = "); opnd_str(b, &in->a, lang); break;
    case I_BIN:
        opnd_str(b, &in->d, lang); bputs(b, " = "); opnd_str(b, &in->a, lang);
        bprintf(b, " %s ", binop_sym((BinOp)in->sub)); opnd_str(b, &in->b, lang);
        break;
    case I_UN:
        opnd_str(b, &in->d, lang); bputs(b, " = ");
        if (in->sub == U_TOSTR || in->sub == U_REPR || in->sub == U_LEN) { bprintf(b, "%s(", unop_sym((UnOp)in->sub)); opnd_str(b, &in->a, lang); bputc(b, ')'); }
        else { bputs(b, unop_sym((UnOp)in->sub)); if (in->sub > U_POS) bputc(b, ' '); opnd_str(b, &in->a, lang); }
        break;
    case I_LABEL: bprintf(b, "L%d:", in->lab); break;
    case I_GOTO:  bprintf(b, "goto L%d", in->lab); break;
    case I_IF:    bputs(b, "if "); opnd_str(b, &in->a, lang); bprintf(b, " goto L%d", in->lab); break;
    case I_IFNOT: bputs(b, "ifFalse "); opnd_str(b, &in->a, lang); bprintf(b, " goto L%d", in->lab); break;
    case I_PARAM: bputs(b, "param "); opnd_str(b, &in->a, lang); break;
    case I_CALL:
        if (in->d.k) { opnd_str(b, &in->d, lang); bputs(b, " = "); }
        bprintf(b, "call %s, %d", in->fn, in->nargs);
        break;
    case I_RET:   bputs(b, "return"); if (in->a.k) { bputc(b, ' '); opnd_str(b, &in->a, lang); } break;
    case I_LOAD:  opnd_str(b, &in->d, lang); bputs(b, " = "); opnd_str(b, &in->a, lang); bputc(b, '['); opnd_str(b, &in->b, lang); bputc(b, ']'); break;
    case I_STORE: opnd_str(b, &in->a, lang); bputc(b, '['); opnd_str(b, &in->b, lang); bputs(b, "] = "); opnd_str(b, &in->c, lang); break;
    case I_NEWARR:
        opnd_str(b, &in->d, lang);
        if (in->c.i == 1) { bputs(b, " = newlist "); opnd_str(b, &in->a, lang); }
        else if (in->c.i == 2) { bputs(b, " = newtuple "); opnd_str(b, &in->a, lang); }
        else {
            bprintf(b, " = new %s[", ty_name((Ty)in->ty)); opnd_str(b, &in->a, lang); bputc(b, ']');
            if (in->b.k) { bputc(b, '['); opnd_str(b, &in->b, lang); bputc(b, ']'); }
        }
        break;
    case I_OPAQUE:
        if (in->d.k) { opnd_str(b, &in->d, lang); bputs(b, " = "); }
        bputs(b, "opaque "); bjson_cstr(b, in->fn);
        break;
    default: bputs(b, "nop");
    }
}

void ir_print(Buf *b, const IRProg *p) {
    for (int f = 0; f < p->f.n; f++) {
        const Func *fn = &p->f.v[f];
        bprintf(b, "function %s(", fn->name);
        for (int i = 0; i < fn->params.n; i++) bprintf(b, "%s%s", i ? ", " : "", fn->params.v[i]);
        bputs(b, "):\n");
        for (int i = 0; i < fn->code.n; i++) {
            const Instr *in = &fn->code.v[i];
            if (in->op == I_NOP) continue;
            bputs(b, in->op == I_LABEL ? "" : "    ");
            instr_str(b, in, p->lang);
            bputc(b, '\n');
        }
        bputc(b, '\n');
    }
}

Opnd *instr_def(Instr *in) {
    switch (in->op) {
    case I_COPY: case I_BIN: case I_UN: case I_LOAD: case I_NEWARR: return &in->d;
    case I_CALL: case I_OPAQUE: return in->d.k ? &in->d : NULL;
    default: return NULL;
    }
}

int instr_uses(Instr *in, Opnd **out) {
    int n = 0;
    switch (in->op) {
    case I_COPY: case I_UN: case I_IF: case I_IFNOT: case I_PARAM: out[n++] = &in->a; break;
    case I_RET: if (in->a.k) out[n++] = &in->a; break;
    case I_BIN: case I_LOAD: out[n++] = &in->a; out[n++] = &in->b; break;
    case I_STORE: out[n++] = &in->a; out[n++] = &in->b; out[n++] = &in->c; break;
    case I_NEWARR: out[n++] = &in->a; if (in->b.k) out[n++] = &in->b; break;
    default: break;
    }
    return n;
}

int instr_count(const IRProg *p) {
    int c = 0;
    for (int f = 0; f < p->f.n; f++)
        for (int i = 0; i < p->f.v[f].code.n; i++) {
            int op = p->f.v[f].code.v[i].op;
            c += op != I_LABEL && op != I_NOP;
        }
    return c;
}

Func *ir_find(IRProg *p, const char *name) {
    for (int i = 0; i < p->f.n; i++) if (strcmp(p->f.v[i].name, name) == 0) return &p->f.v[i];
    return NULL;
}

void ir_free(IRProg *p) {
    for (int i = 0; i < p->f.n; i++) { vfree(p->f.v[i].params); vfree(p->f.v[i].ptys); vfree(p->f.v[i].code); }
    vfree(p->f); vfree(p->globals); vfree(p->entry);
}
