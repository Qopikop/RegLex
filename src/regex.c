#include "regex.h"
#include <ctype.h>

/* ---- parser: recursive descent straight to postfix ----
 *
 *   alt    := concat ('|' concat)*
 *   concat := repeat*                  (no items -> the empty string)
 *   repeat := atom ('*' | '+' | '?')*
 *   atom   := '(' alt ')' | '[' class ']' | '.' | '\' byte | byte
 *
 * Emitting operands before their operator gives postfix directly, so the
 * precedence (repetition > concatenation > alternation) is in the grammar
 * rather than in an operator stack. */

typedef struct {
    const char *re;
    int i, depth;
    NFA *nfa;
    Postfix *pf;
    char *err;
    size_t errlen;
    int failed;
} RP;

#define MAX_NESTING 200

static void rp_fail(RP *p, const char *fmt, int pos) {
    if (p->failed) return;
    p->failed = 1;
    char what[160];
    snprintf(what, sizeof(what), fmt, pos + 1);
    snprintf(p->err, p->errlen, "%s", what);
}

static void emit(RP *p, PfKind k, int v) { PfItem it = { (unsigned char)k, v }; vpush(*p->pf, it); }

static int escape_byte(int c) {
    switch (c) {
    case 'n': return '\n'; case 't': return '\t'; case 'r': return '\r';
    case 'f': return '\f'; case 'v': return '\v';
    default:  return c;
    }
}

static int new_class(RP *p) { CSet z; memset(&z, 0, sizeof(z)); vpush(p->nfa->cls, z); return p->nfa->cls.n - 1; }

/* At p->re[p->i] == '['. A ']' right after '[' or '[^' is a literal. */
static void parse_class(RP *p) {
    int open = p->i++;
    int idx = new_class(p);
    CSet set; memset(&set, 0, sizeof(set));
    int negate = 0;
    if (p->re[p->i] == '^') { negate = 1; p->i++; }
    int first = 1;
    while (p->re[p->i] && (p->re[p->i] != ']' || first)) {
        first = 0;
        int lo = (unsigned char)p->re[p->i];
        if (lo == '\\') {
            if (!p->re[p->i + 1]) { rp_fail(p, "pattern ends with a lone backslash (position %d)", p->i); return; }
            lo = escape_byte((unsigned char)p->re[++p->i]);
        }
        if (p->re[p->i + 1] == '-' && p->re[p->i + 2] && p->re[p->i + 2] != ']') {
            int j = p->i + 2;
            int hi = (unsigned char)p->re[j];
            if (hi == '\\' && p->re[j + 1]) hi = escape_byte((unsigned char)p->re[++j]);
            if (hi < lo) { rp_fail(p, "backwards range in character class at position %d", p->i); return; }
            for (int c = lo; c <= hi; c++) cset_add(&set, c);
            p->i = j + 1;
        } else {
            cset_add(&set, lo);
            p->i++;
        }
    }
    if (p->re[p->i] != ']') { rp_fail(p, "character class opened at position %d is never closed with ']'", open); return; }
    p->i++;
    if (negate) for (int b = 0; b < 32; b++) set.b[b] = (unsigned char)~set.b[b];
    set.b[0] &= (unsigned char)~1u;      /* byte 0 ends the input; never match it */
    p->nfa->cls.v[idx] = set;
    emit(p, PF_CLASS, idx);
}

static void parse_alt(RP *p);

static int parse_atom(RP *p) {
    char c = p->re[p->i];
    if (c == '(') {
        int open = p->i++;
        if (++p->depth > MAX_NESTING) { rp_fail(p, "groups nested too deeply (position %d)", open); return 0; }
        parse_alt(p);
        p->depth--;
        if (p->failed) return 0;
        if (p->re[p->i] != ')') { rp_fail(p, "'(' at position %d has no matching ')'", open); return 0; }
        p->i++;
    } else if (c == '[') {
        parse_class(p);
    } else if (c == '.') {
        int idx = new_class(p);
        for (int b = 1; b < 256; b++) if (b != '\n') cset_add(&p->nfa->cls.v[idx], b);
        p->i++;
        emit(p, PF_CLASS, idx);
    } else if (c == '\\') {
        if (!p->re[p->i + 1]) { rp_fail(p, "pattern ends with a lone backslash (position %d)", p->i); return 0; }
        emit(p, PF_LIT, escape_byte((unsigned char)p->re[p->i + 1]));
        p->i += 2;
    } else {
        emit(p, PF_LIT, (unsigned char)c);
        p->i++;
    }
    return !p->failed;
}

static void parse_repeat(RP *p) {
    if (!parse_atom(p)) return;
    while (p->re[p->i] == '*' || p->re[p->i] == '+' || p->re[p->i] == '?') {
        char q = p->re[p->i++];
        emit(p, q == '*' ? PF_STAR : q == '+' ? PF_PLUS : PF_OPT, 0);
    }
}

static void parse_concat(RP *p) {
    int items = 0;
    while (!p->failed && p->re[p->i] && p->re[p->i] != '|' && p->re[p->i] != ')') {
        char c = p->re[p->i];
        if (c == '*' || c == '+' || c == '?') { rp_fail(p, "'%c' has nothing to repeat", 0);
            snprintf(p->err, p->errlen, "'%c' at position %d has nothing to repeat", c, p->i + 1); return; }
        parse_repeat(p);
        if (++items > 1) emit(p, PF_CAT, 0);
    }
    if (items == 0) emit(p, PF_EPS, 0);
}

static void parse_alt(RP *p) {
    parse_concat(p);
    while (!p->failed && p->re[p->i] == '|') {
        p->i++;
        parse_concat(p);
        emit(p, PF_ALT, 0);
    }
}

int regex_to_postfix(const char *re, NFA *nfa, Postfix *pf, char *err, size_t errlen) {
    RP p = { re, 0, 0, nfa, pf, err, errlen, 0 };
    if (!re[0]) { snprintf(err, errlen, "empty pattern (it would match nothing but the empty string)"); return -1; }
    parse_alt(&p);
    if (!p.failed && re[p.i] == ')') rp_fail(&p, "')' at position %d has no matching '('", p.i);
    return p.failed ? -1 : 0;
}

/* ---- Thompson's construction ----
 * A fragment is a start state plus the list of its still-dangling exits
 * (which `out` field of which state), kept in a small linked arena. */

typedef struct { int code, next; } Dangle;       /* code = state*2 + (0: out, 1: out1) */
typedef struct { int start, head, tail; } Frag;
typedef VEC(Dangle) DangleVec;

static int dangle(DangleVec *dl, int state, int which) {
    Dangle d = { state * 2 + which, -1 };
    vpush(*dl, d);
    return dl->n - 1;
}

static int add_state(NFA *n, int type, int c, int out, int out1) {
    NState s = { (unsigned char)type, c, out, out1, -1 };
    vpush(n->s, s);
    return n->s.n - 1;
}

int nfa_from_postfix(NFA *nfa, const Postfix *pf, int rule) {
    DangleVec dl = {0};
    VEC(Frag) st = {0};
#define PATCH(head, target) for (int k_ = (head); k_ >= 0; k_ = dl.v[k_].next) { \
        int s_ = dl.v[k_].code >> 1; if (dl.v[k_].code & 1) nfa->s.v[s_].out1 = (target); else nfa->s.v[s_].out = (target); }
    for (int i = 0; i < pf->n; i++) {
        PfItem it = pf->v[i];
        Frag f;
        switch (it.k) {
        case PF_LIT: case PF_CLASS: {
            int s = add_state(nfa, it.k == PF_LIT ? NS_CHAR : NS_CLASS, it.v, -1, -1);
            int d = dangle(&dl, s, 0);
            f = (Frag){ s, d, d };
            break;
        }
        case PF_EPS: {
            int s = add_state(nfa, NS_SPLIT, 0, -1, -1);
            int d = dangle(&dl, s, 0);
            f = (Frag){ s, d, d };
            break;
        }
        case PF_CAT: {
            Frag e2 = st.v[--st.n], e1 = st.v[--st.n];
            PATCH(e1.head, e2.start);
            f = (Frag){ e1.start, e2.head, e2.tail };
            break;
        }
        case PF_ALT: {
            Frag e2 = st.v[--st.n], e1 = st.v[--st.n];
            int s = add_state(nfa, NS_SPLIT, 0, e1.start, e2.start);
            dl.v[e1.tail].next = e2.head;
            f = (Frag){ s, e1.head, e2.tail };
            break;
        }
        case PF_STAR: {
            Frag e = st.v[--st.n];
            int s = add_state(nfa, NS_SPLIT, 0, e.start, -1);
            PATCH(e.head, s);
            int d = dangle(&dl, s, 1);
            f = (Frag){ s, d, d };
            break;
        }
        case PF_PLUS: {
            Frag e = st.v[--st.n];
            int s = add_state(nfa, NS_SPLIT, 0, e.start, -1);
            PATCH(e.head, s);
            int d = dangle(&dl, s, 1);
            f = (Frag){ e.start, d, d };
            break;
        }
        default: { /* PF_OPT */
            Frag e = st.v[--st.n];
            int s = add_state(nfa, NS_SPLIT, 0, e.start, -1);
            int d = dangle(&dl, s, 1);
            dl.v[e.tail].next = d;
            f = (Frag){ s, e.head, d };
            break;
        }
        }
        vpush(st, f);
    }
    int m = add_state(nfa, NS_MATCH, 0, -1, -1);
    nfa->s.v[m].rule = rule;
    PATCH(st.v[0].head, m);
    int start = st.v[0].start;
#undef PATCH
    vfree(dl);
    vfree(st);
    return start;
}

int regex_compile(NFA *nfa, const char *re, int rule, Postfix *pf, char *err, size_t errlen) {
    Postfix local = {0};
    Postfix *use = pf ? pf : &local;
    int ncls = nfa->cls.n;
    if (regex_to_postfix(re, nfa, use, err, errlen) < 0) {
        nfa->cls.n = ncls;          /* drop classes from the failed pattern */
        if (!pf) vfree(local);
        return -1;
    }
    int start = nfa_from_postfix(nfa, use, rule);
    if (!pf) vfree(local);
    return start;
}

void nfa_free(NFA *nfa) { vfree(nfa->s); vfree(nfa->cls); }

/* ---- labels ---- */

const char *show_byte(int c, char *buf) {
    if (c == '\n') return "\\n";
    if (c == '\t') return "\\t";
    if (c == '\r') return "\\r";
    if (c == '\f') return "\\f";
    if (c == '\v') return "\\v";
    if (c == ' ')  return "\xe2\x90\xa3";   /* the visible-space symbol */
    if (c > 32 && c < 127) { buf[0] = (char)c; buf[1] = 0; return buf; }
    snprintf(buf, 8, "\\x%02x", c);
    return buf;
}

void cset_label(const CSet *s, char *buf, size_t n) {
    int count = 0;
    for (int c = 1; c < 256; c++) count += cset_test(s, c);
    int inv = count > 128;                      /* mostly-full sets read better as [^...] */
    size_t k = 0;
    char tmp[8];
#define PUT(str) do { const char *p_ = (str); while (*p_ && k + 1 < n) buf[k++] = *p_++; } while (0)
    PUT("[");
    if (inv) PUT("^");
    for (int c = 1; c < 256 && k + 8 < n; ) {
        if (cset_test(s, c) == inv) { c++; continue; }
        int lo = c;
        while (c + 1 < 256 && cset_test(s, c + 1) != inv) c++;
        int hi = c++;
        if (k > 48) { PUT("\xe2\x80\xa6"); break; }
        if (lo && strchr("-]^\\", lo)) PUT("\\");
        PUT(lo == ' ' ? " " : show_byte(lo, tmp));
        if (hi > lo) {
            if (hi > lo + 1) PUT("-");
            if (hi && strchr("-]^\\", hi)) PUT("\\");
            PUT(hi == ' ' ? " " : show_byte(hi, tmp));
        }
    }
    PUT("]");
    buf[k] = 0;
#undef PUT
}
