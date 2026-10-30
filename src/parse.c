#include "parse.h"
#include <ctype.h>
#include <errno.h>

Token *P_cur(Parser *p) { return &p->t->v[p->p]; }
Token *P_peek(Parser *p, int ahead) {
    int i = p->p + ahead;
    return &p->t->v[i < p->t->n ? i : p->t->n - 1];
}
int P_is(Parser *p, const char *text) { return strcmp(P_cur(p)->text, text) == 0 && P_cur(p)->kind != TK_STRING; }
int P_is_at(Parser *p, int ahead, const char *text) {
    Token *t = P_peek(p, ahead);
    return strcmp(t->text, text) == 0 && t->kind != TK_STRING;
}
int P_accept(Parser *p, const char *text) {
    if (!P_is(p, text)) return 0;
    if (P_cur(p)->kind != TK_EOF) p->p++;
    return 1;
}

static const char *describe(Token *t) {
    static char buf[96];
    if (t->kind == TK_EOF) return "end of file";
    if (t->kind == TK_NEWLINE) return "end of line";
    if (t->kind == TK_INDENT) return "an indented block";
    if (t->kind == TK_DEDENT) return "the end of the block";
    snprintf(buf, sizeof(buf), "'%.40s'", t->text);
    return buf;
}

void P_error(Parser *p, Token *at, const char *category, const char *hint, const char *fmt, ...) {
    if (p->panic) return;
    p->panic = 1;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    diag(p->ds, SEV_ERROR, "parser", category, at->line, at->col, hint, "%s", msg);
}

int P_expect(Parser *p, const char *text, const char *where) {
    if (P_accept(p, text)) return 1;
    Token *t = P_cur(p);
    char hint[128];
    snprintf(hint, sizeof(hint), "Insert '%s' %s.", text, where);
    /* point at the end of the previous token: that is where the ';' belongs */
    Token *at = t;
    Token prevpos;
    if (p->p > 0 && (strcmp(text, ";") == 0 || strcmp(text, ":") == 0)) {
        prevpos = p->t->v[p->p - 1];
        prevpos.col += prevpos.len;
        at = &prevpos;
    }
    P_error(p, at, "Syntax Error", hint, "expected '%s' %s, found %s", text, where, describe(t));
    return 0;
}

void P_trace(Parser *p, const char *fmt, ...) {
    if (p->trace.n >= p->trace_limit) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    vpush(p->trace, xstrdup(msg));
}

int P_enter(Parser *p) {
    if (++p->depth > 400) {
        P_error(p, P_cur(p), "Nesting Too Deep", "Split the expression or block into smaller pieces.",
                "nesting is deeper than the parser's limit of 400 levels");
        return 0;
    }
    return 1;
}
void P_leave(Parser *p) { p->depth--; }

Node *P_node(Parser *p, NodeKind k) { Token *t = P_cur(p); return node_new(k, t->line, t->col); }

Node *parse_program(Parser *p) {
    p->trace_limit = p->trace_limit ? p->trace_limit : 400;
    return p->lang == LANG_PY ? parse_python(p) : parse_c_family(p);
}

/* ---- literals ---- */

long long parse_int_literal(const char *text, int *ok) {
    char buf[96];
    int k = 0;
    for (int i = 0; text[i] && k < 94; i++) if (text[i] != '_') buf[k++] = text[i];
    buf[k] = 0;
    while (k > 0 && strchr("uUlLjJ", buf[k - 1])) buf[--k] = 0;   /* suffixes */
    int base = 10;
    const char *s = buf;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) { base = 2; s += 2; }
    else if (s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) { base = 8; s += 2; }
    else if (s[0] == '0' && s[1]) base = 8;                          /* C/Java octal */
    errno = 0;
    char *end;
    unsigned long long v = strtoull(s, &end, base);
    if (ok) *ok = errno == 0 && *end == 0;
    return (long long)v;
}

int is_float_literal(const char *text, Lang lang) {
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) return 0;
    if (strchr(text, '.') || strchr(text, 'e') || strchr(text, 'E')) return 1;
    size_t n = strlen(text);
    if (lang != LANG_PY && n && strchr("fFdD", text[n - 1])) return 1;
    return 0;
}

double parse_float_literal(const char *text) {
    char buf[96];
    int k = 0;
    for (int i = 0; text[i] && k < 94; i++) if (text[i] != '_') buf[k++] = text[i];
    buf[k] = 0;
    return strtod(buf, NULL);
}

static int hexval(int c) { return isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10); }

/* Decodes one escape at s[i] == '\\'; appends to b; returns chars consumed. */
static int decode_escape(const char *s, Buf *b) {
    char c = s[1];
    switch (c) {
    case 'n': bputc(b, '\n'); return 2;
    case 't': bputc(b, '\t'); return 2;
    case 'r': bputc(b, '\r'); return 2;
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
        int v = 0, k = 1;
        while (k <= 3 && s[k] >= '0' && s[k] <= '7') v = v * 8 + (s[k++] - '0');
        bputc(b, (char)v);
        return k;
    }
    case 'x': {
        int v = 0, k = 2;
        while (isxdigit((unsigned char)s[k]) && k < 4) v = v * 16 + hexval(s[k++]);
        bputc(b, (char)v);
        return k;
    }
    case 'u': {
        int k = 2;
        while (s[k] == 'u') k++;               /* Java allows \uuuu0041 */
        unsigned v = 0;
        int d = 0;
        while (d < 4 && isxdigit((unsigned char)s[k])) { v = v * 16 + (unsigned)hexval(s[k++]); d++; }
        if (v < 0x80) bputc(b, (char)v);
        else if (v < 0x800) { bputc(b, (char)(0xC0 | (v >> 6))); bputc(b, (char)(0x80 | (v & 63))); }
        else { bputc(b, (char)(0xE0 | (v >> 12))); bputc(b, (char)(0x80 | ((v >> 6) & 63))); bputc(b, (char)(0x80 | (v & 63))); }
        return k;
    }
    case 'a': bputc(b, '\a'); return 2;
    case 'b': bputc(b, '\b'); return 2;
    case 'f': bputc(b, '\f'); return 2;
    case 'v': bputc(b, '\v'); return 2;
    case '\n': return 2;                        /* line continuation inside a string */
    default: bputc(b, c); return 2;             /* \\ \" \' and unknown escapes */
    }
}

const char *decode_string(const char *text, Lang lang, int *is_fstring, int *is_bytes) {
    int raw = 0, f = 0, bytes = 0;
    const char *s = text;
    while (*s && *s != '"' && *s != '\'') {                 /* prefixes: r b f u L u8 */
        char c = (char)tolower((unsigned char)*s);
        if (c == 'r') raw = 1;
        if (c == 'f') f = 1;
        if (c == 'b') bytes = 1;
        s++;
    }
    if (lang != LANG_PY) raw = 0;
    char q = *s;
    int ql = (s[0] && s[1] == q && s[2] == q) ? 3 : 1;
    size_t n = strlen(s);
    if (n < (size_t)(2 * ql)) ql = 1;
    const char *body = s + ql, *end = s + n - ql;
    if (ql == 3 && lang == LANG_JAVA) {                  /* text block: drop the first newline */
        if (*body == '\n') body++;
    }
    Buf b = {0};
    for (const char *c = body; c < end; ) {
        if (*c == '\\' && !raw && c + 1 < end + (ql == 3 ? 0 : 1)) c += decode_escape(c, &b);
        else bputc(&b, *c++);
    }
    if (is_fstring) *is_fstring = f;
    if (is_bytes) *is_bytes = bytes;
    const char *r = intern_n(b.s ? b.s : "", b.n);
    free(b.s);
    return r;
}

long long decode_char(const char *text) {
    const char *s = strchr(text, '\'');
    if (!s) return 0;
    Buf b = {0};
    if (s[1] == '\\') decode_escape(s + 1, &b);
    else bputc(&b, s[1]);
    long long v = b.n ? (unsigned char)b.s[0] : 0;
    if (b.n > 1 && ((unsigned char)b.s[0] & 0xE0) == 0xC0) v = ((b.s[0] & 31) << 6) | (b.s[1] & 63);
    free(b.s);
    return v;
}
