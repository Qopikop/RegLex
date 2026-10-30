/* Python parser. Blocks come from the layout tokens (NEWLINE INDENT ...
 * DEDENT) that python_layout adds to the DFA's token stream.
 *
 *   test       := or_test ('if' or_test 'else' test)? | lambda
 *   or_test    := and_test ('or' and_test)*
 *   and_test   := not_test ('and' not_test)*
 *   not_test   := 'not' not_test | comparison
 *   comparison := bitor (COMP_OP bitor)*         chains: a < b <= c
 *   bitor .. term : the usual binary levels      | ^ & << >> + - * / // % @
 *   factor     := ('+'|'-'|'~') factor | power
 *   power      := primary ('**' factor)?          right-associative
 *   primary    := atom ('(' args ')' | '[' subscript ']' | '.' NAME)*
 */
#include "parse.h"

static Node *test(Parser *p);
static Node *statement(Parser *p);
static Node *block_suite(Parser *p);
enum { TL_VALUE, TL_STAR, TL_TARGET };   /* testlist modes */
static Node *testlist(Parser *p, int mode);
static Node *binary(Parser *p, int min);
static Node *or_test(Parser *p);

static Node *mk(NodeKind k, Token *t, const char *s) { Node *n = node_new(k, t->line, t->col); n->s = s; return n; }

static int at_newline(Parser *p) { TokKind k = P_cur(p)->kind; return k == TK_NEWLINE || k == TK_EOF; }

/* Skips to the end of the logical line (and any block that follows it). */
static void sync_line(Parser *p) {
    while (!at_newline(p) && P_cur(p)->kind != TK_DEDENT) p->p++;
    if (P_cur(p)->kind == TK_NEWLINE) p->p++;
    if (P_cur(p)->kind == TK_INDENT) {
        int depth = 0;
        do {
            if (P_cur(p)->kind == TK_INDENT) depth++;
            else if (P_cur(p)->kind == TK_DEDENT) depth--;
            if (P_cur(p)->kind == TK_EOF) break;
            p->p++;
        } while (depth > 0);
    }
    p->panic = 0;
}

static Node *unsupported(Token *t, const char *what) { return mk(N_Unsupported, t, intern(what)); }

/* names the scanner marks as keywords but that act as identifiers here */
static int soft_name(Token *t) {
    return t->kind == TK_KEYWORD && (strcmp(t->text, "self") == 0 || strcmp(t->text, "match") == 0 ||
                                     strcmp(t->text, "case") == 0 || strcmp(t->text, "print") == 0);
}
static int is_name(Token *t) { return t->kind == TK_IDENT || soft_name(t); }

/* ---- f-strings ---- */

static Node *fstring(Parser *p, Token *t, const char *body) {
    Node *f = mk(N_FString, t, NULL);
    Buf lit = {0};
    for (const char *c = body; *c; ) {
        if (c[0] == '{' && c[1] == '{') { bputc(&lit, '{'); c += 2; continue; }
        if (c[0] == '}' && c[1] == '}') { bputc(&lit, '}'); c += 2; continue; }
        if (c[0] != '{') { bputc(&lit, *c++); continue; }
        if (lit.n) { node_add(f, mk(N_StrLit, t, intern_n(lit.s, lit.n))); lit.n = 0; }
        /* find the matching '}' (brackets and quotes inside the expression are allowed) */
        const char *e = ++c;
        int depth = 0;
        char quote = 0;
        const char *conv = NULL, *spec = NULL;
        while (*c) {
            if (quote) { if (*c == quote) quote = 0; c++; continue; }
            if (*c == '\'' || *c == '"') { quote = *c++; continue; }
            if (strchr("([{", *c)) depth++;
            else if (strchr(")]", *c)) depth--;
            else if (*c == '}') { if (depth == 0) break; depth--; }
            else if (depth == 0 && *c == '!' && c[1] != '=') { conv = c; }
            else if (depth == 0 && *c == ':' && !spec) { spec = c; }
            c++;
        }
        const char *expr_end = conv ? conv : spec ? spec : c;
        char *src = xstrndup(e, (size_t)(expr_end - e));
        Node *part = mk(N_FormatPart, t, NULL);
        if (spec) part->s = intern_n(spec + 1, (size_t)(c - spec - 1));
        if (conv && conv[1] == 'r') part->type = intern("repr");
        /* scan and parse the expression with the same DFA and parser */
        Tokens raw = {0}, toks = {0};
        Diags sub = {0};
        lex(p->rs, src, SCAN_MIN, &raw, &sub, 0);
        plain_layout(&raw, &toks);
        Parser q = { &toks, 0, &sub, LANG_PY, 0, 0, p->rs, src, {0}, {0}, 0 };
        Node *x = test(&q);
        if (sub.errors || P_cur(&q)->kind != TK_EOF) {
            diag(p->ds, SEV_ERROR, "parser", "Syntax Error", t->line, t->col, NULL,
                 "invalid expression '%s' inside an f-string", src);
            node_free(x);
            x = unsupported(t, "f-string expression");
        }
        x->line = t->line; x->col = t->col;
        node_add(part, x);
        node_add(f, part);
        free(src);
        vfree(raw); vfree(toks);
        for (int i = 0; i < sub.d.n; i++) { free(sub.d.v[i].msg); free(sub.d.v[i].hint); }
        vfree(sub.d);
        if (*c == '}') c++;
    }
    if (lit.n || !f->k.n) node_add(f, mk(N_StrLit, t, intern_n(lit.s ? lit.s : "", lit.n)));
    free(lit.s);
    return f;
}

/* ---- atoms ---- */

static Node *strings(Parser *p) {
    Token *t = P_cur(p);
    Node *result = NULL;
    while (P_cur(p)->kind == TK_STRING) {
        Token *s = P_cur(p);
        int is_f = 0, is_b = 0;
        const char *val = decode_string(s->text, LANG_PY, &is_f, &is_b);
        p->p++;
        Node *part = is_f ? fstring(p, s, val) : mk(N_StrLit, s, val);
        if (!result) { result = part; continue; }
        /* implicit concatenation: "a" "b" or "a" f"{x}" */
        if (result->kind == N_StrLit && part->kind == N_StrLit) {
            Buf b = {0}; bputs(&b, result->s); bputs(&b, part->s);
            result->s = intern(b.s); free(b.s); node_free(part);
            continue;
        }
        if (result->kind != N_FString) { Node *f = mk(N_FString, t, NULL); node_add(f, result); result = f; }
        if (part->kind == N_FString) { for (int i = 0; i < part->k.n; i++) node_add(result, part->k.v[i]); part->k.n = 0; node_free(part); }
        else node_add(result, part);
    }
    return result;
}

static Node *comprehension_tail(Parser *p, Token *t, const char *kind, Node *elem) {
    Node *c = mk(N_Comprehension, t, intern(kind));
    node_add(c, elem);
    while (P_is(p, "for") || P_is(p, "async")) {
        P_accept(p, "async");
        p->p++;
        node_add(c, testlist(p, TL_TARGET));
        P_expect(p, "in", "in the comprehension");
        node_add(c, or_test(p));                       /* iterable: no bare conditional here */
        while (P_is(p, "if")) { p->p++; node_add(c, or_test(p)); }
    }
    return c;
}

static Node *atom(Parser *p) {
    Token *t = P_cur(p);
    if (t->kind == TK_NUMBER) {
        p->p++;
        size_t L = strlen(t->text);
        if (L && (t->text[L - 1] == 'j' || t->text[L - 1] == 'J')) return unsupported(t, "complex number");
        if (is_float_literal(t->text, LANG_PY)) { Node *n = mk(N_FloatLit, t, t->text); n->fval = parse_float_literal(t->text); return n; }
        int ok;
        Node *n = mk(N_IntLit, t, t->text);
        n->ival = parse_int_literal(t->text, &ok);
        if (!ok || n->ival < 0) { node_free(n); return unsupported(t, "integer literal beyond 64 bits"); }
        return n;
    }
    if (t->kind == TK_STRING) return strings(p);
    if (is_name(t)) { p->p++; return mk(N_Name, t, t->text); }
    if (P_is(p, "True") || P_is(p, "False")) { p->p++; Node *n = mk(N_BoolLit, t, t->text); n->ival = t->text[0] == 'T'; return n; }
    if (P_is(p, "None")) { p->p++; return mk(N_NullLit, t, t->text); }
    if (P_is(p, "...")) { p->p++; return unsupported(t, "Ellipsis"); }
    if (P_is(p, "(")) {
        p->p++;
        if (P_accept(p, ")")) return mk(N_Tuple, t, NULL);
        Node *first = P_is(p, "*") ? NULL : test(p);
        if (first && (P_is(p, "for") || P_is(p, "async"))) {
            Node *c = comprehension_tail(p, t, "gen", first);
            P_expect(p, ")", "to close the generator expression");
            return c;
        }
        if (first && P_is(p, ")")) { p->p++; return first; }   /* parenthesized expression */
        Node *tup = mk(N_Tuple, t, NULL);
        if (first) { node_add(tup, first); P_accept(p, ","); }
        while (!P_is(p, ")") && !at_newline(p) && !p->panic) {
            if (P_is(p, "*")) { Token *s = P_cur(p); p->p++; Node *st = mk(N_Star, s, NULL); node_add(st, test(p)); node_add(tup, st); }
            else node_add(tup, test(p));
            if (!P_accept(p, ",")) break;
        }
        P_expect(p, ")", "to close the tuple");
        return tup;
    }
    if (P_is(p, "[")) {
        p->p++;
        Node *list = mk(N_ArrayInit, t, NULL);
        if (P_accept(p, "]")) return list;
        Node *first = test(p);
        if (P_is(p, "for") || P_is(p, "async")) {
            node_free(list);
            Node *c = comprehension_tail(p, t, "list", first);
            P_expect(p, "]", "to close the list comprehension");
            return c;
        }
        node_add(list, first);
        while (P_accept(p, ",") && !P_is(p, "]")) {
            if (P_is(p, "*")) { Token *s = P_cur(p); p->p++; Node *st = mk(N_Star, s, NULL); node_add(st, test(p)); node_add(list, st); }
            else node_add(list, test(p));
        }
        P_expect(p, "]", "to close the list");
        return list;
    }
    if (P_is(p, "{")) {
        p->p++;
        Node *d = mk(N_Dict, t, NULL);
        if (P_accept(p, "}")) return d;
        if (P_is(p, "**")) { p->p++; node_add(d, test(p)); }
        else {
            Node *k = test(p);
            if (P_accept(p, ":")) {
                Node *v = test(p);
                if (P_is(p, "for")) {
                    Node *pair = mk(N_Tuple, t, NULL); node_add(pair, k); node_add(pair, v);
                    node_free(d);
                    Node *c = comprehension_tail(p, t, "dict", pair);
                    P_expect(p, "}", "to close the dict comprehension");
                    return c;
                }
                node_add(d, k); node_add(d, v);
                while (P_accept(p, ",") && !P_is(p, "}")) {
                    if (P_is(p, "**")) { p->p++; node_add(d, test(p)); continue; }
                    node_add(d, test(p));
                    P_expect(p, ":", "between dict key and value");
                    node_add(d, test(p));
                }
            } else {                                          /* set */
                d->s = intern("set");
                if (P_is(p, "for")) { node_free(d); Node *c = comprehension_tail(p, t, "set", k); P_expect(p, "}", "to close the set comprehension"); return c; }
                node_add(d, k);
                while (P_accept(p, ",") && !P_is(p, "}")) node_add(d, test(p));
            }
        }
        P_expect(p, "}", "to close the dict");
        return d;
    }
    P_error(p, t, "Syntax Error", "Check for a missing operand or an extra operator.",
            "expected an expression, found %s", t->kind == TK_NEWLINE ? "end of line" : t->kind == TK_EOF ? "end of file" : t->text);
    if (!at_newline(p) && t->kind != TK_DEDENT && t->kind != TK_INDENT) p->p++;
    return unsupported(t, "syntax error");
}

static Node *subscript(Parser *p) {
    Token *t = P_cur(p);
    Node *lo = NULL;
    if (!P_is(p, ":")) lo = test(p);
    if (!P_is(p, ":")) return lo;
    Node *s = mk(N_Slice, t, NULL);
    node_add(s, lo ? lo : mk(N_Empty, t, NULL));
    p->p++;
    node_add(s, (P_is(p, "]") || P_is(p, ":") || P_is(p, ",")) ? mk(N_Empty, t, NULL) : test(p));
    if (P_accept(p, ":") && !P_is(p, "]")) node_add(s, test(p));
    else node_add(s, mk(N_Empty, t, NULL));
    return s;
}

static Node *primary(Parser *p) {
    Node *e = atom(p);
    for (;;) {
        Token *t = P_cur(p);
        if (P_is(p, "(")) {
            p->p++;
            Node *c = mk(N_Call, t, NULL);
            c->line = e->line; c->col = e->col;
            node_add(c, e);
            while (!P_is(p, ")") && !at_newline(p) && !p->panic) {
                Token *a = P_cur(p);
                if (P_is(p, "*") || P_is(p, "**")) { p->p++; Node *st = mk(N_Star, a, a->text); node_add(st, test(p)); node_add(c, st); }
                else if (is_name(a) && P_is_at(p, 1, "=")) { p->p += 2; Node *kw = mk(N_KwArg, a, a->text); node_add(kw, test(p)); node_add(c, kw); }
                else {
                    Node *arg = test(p);
                    if (P_is(p, "for")) arg = comprehension_tail(p, a, "gen", arg);
                    node_add(c, arg);
                }
                if (!P_accept(p, ",")) break;
            }
            P_expect(p, ")", "to close the call");
            e = c;
        } else if (P_is(p, "[")) {
            p->p++;
            Node *ix = mk(N_Index, t, NULL);
            node_add(ix, e);
            Node *sub = subscript(p);
            if (P_is(p, ",")) {
                Node *tup = mk(N_Tuple, t, NULL); node_add(tup, sub);
                while (P_accept(p, ",") && !P_is(p, "]")) node_add(tup, subscript(p));
                sub = tup;
            }
            node_add(ix, sub);
            P_expect(p, "]", "to close the subscript");
            e = ix;
        } else if (P_is(p, ".")) {
            p->p++;
            Token *f = P_cur(p);
            if (f->kind != TK_IDENT && f->kind != TK_KEYWORD) { P_error(p, f, "Syntax Error", NULL, "expected an attribute name after '.'"); return e; }
            p->p++;
            Node *m = mk(N_Member, f, f->text);
            node_add(m, e);
            e = m;
        } else return e;
    }
}

static Node *factor(Parser *p);
static Node *power(Parser *p) {
    Token *t = P_cur(p);
    if (P_is(p, "await")) { p->p++; Node *u = unsupported(t, "await"); node_free(primary(p)); return u; }
    Node *b = primary(p);
    if (P_is(p, "**")) {
        Token *op = P_cur(p);
        p->p++;
        Node *n = mk(N_Binary, op, op->text);
        node_add(n, b);
        node_add(n, factor(p));
        return n;
    }
    return b;
}

static Node *factor(Parser *p) {
    Token *t = P_cur(p);
    if (P_is(p, "-") || P_is(p, "+") || P_is(p, "~")) {
        p->p++;
        if (!P_enter(p)) { P_leave(p); return unsupported(t, "too deep"); }
        Node *n = mk(N_Unary, t, t->text);
        node_add(n, factor(p));
        P_leave(p);
        return n;
    }
    return power(p);
}

static int bin_prec(Token *t) {
    static const struct { const char *op; int prec; } tab[] = {
        {"|", 1}, {"^", 2}, {"&", 3}, {"<<", 4}, {">>", 4}, {"+", 5}, {"-", 5},
        {"*", 6}, {"/", 6}, {"//", 6}, {"%", 6}, {"@", 6} };
    if (t->kind != TK_OP) return 0;
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) if (strcmp(tab[i].op, t->text) == 0) return tab[i].prec;
    return 0;
}

static Node *binary(Parser *p, int min) {
    if (!P_enter(p)) { P_leave(p); return unsupported(P_cur(p), "too deep"); }
    Node *lhs = factor(p);
    for (;;) {
        Token *t = P_cur(p);
        int prec = bin_prec(t);
        if (!prec || prec < min) break;
        p->p++;
        Node *n = mk(N_Binary, t, t->text);
        node_add(n, lhs);
        node_add(n, binary(p, prec + 1));
        lhs = n;
    }
    P_leave(p);
    return lhs;
}

/* comparison operators, including the two-word ones */
static const char *comp_op(Parser *p) {
    static const char *ops[] = { "<", ">", "==", ">=", "<=", "!=", NULL };
    Token *t = P_cur(p);
    for (int i = 0; ops[i]; i++) if (t->kind == TK_OP && strcmp(t->text, ops[i]) == 0) { p->p++; return t->text; }
    if (P_is(p, "in")) { p->p++; return intern("in"); }
    if (P_is(p, "not") && P_is_at(p, 1, "in")) { p->p += 2; return intern("not in"); }
    if (P_is(p, "is")) { p->p++; if (P_accept(p, "not")) return intern("is not"); return intern("is"); }
    return NULL;
}

static Node *comparison(Parser *p) {
    Node *first = binary(p, 1);
    Token *t = P_cur(p);
    const char *op = comp_op(p);
    if (!op) return first;
    Node *c = mk(N_Compare, t, NULL);
    node_add(c, first);
    Buf ops = {0};
    do {
        if (ops.n) bputc(&ops, ',');
        bputs(&ops, op);
        node_add(c, binary(p, 1));
    } while ((op = comp_op(p)));
    c->s = intern(ops.s);
    free(ops.s);
    return c;
}

static Node *not_test(Parser *p) {
    Token *t = P_cur(p);
    if (P_is(p, "not")) {
        p->p++;
        Node *n = mk(N_Unary, t, intern("not"));
        node_add(n, not_test(p));
        return n;
    }
    return comparison(p);
}

static Node *and_test(Parser *p) {
    Node *l = not_test(p);
    while (P_is(p, "and")) {
        Token *t = P_cur(p); p->p++;
        Node *n = mk(N_Logical, t, intern("and")); node_add(n, l); node_add(n, not_test(p)); l = n;
    }
    return l;
}

static Node *or_test(Parser *p) {
    Node *l = and_test(p);
    while (P_is(p, "or")) {
        Token *t = P_cur(p); p->p++;
        Node *n = mk(N_Logical, t, intern("or")); node_add(n, l); node_add(n, and_test(p)); l = n;
    }
    return l;
}

static Node *test(Parser *p) {
    Token *t = P_cur(p);
    if (P_is(p, "lambda")) {
        p->p++;
        while (!P_is(p, ":") && !at_newline(p)) p->p++;
        P_expect(p, ":", "after the lambda parameters");
        Node *l = mk(N_Lambda, t, NULL);
        node_add(l, test(p));
        return l;
    }
    Node *c = or_test(p);
    if (P_is(p, "if") ) {
        Token *it = P_cur(p);
        p->p++;
        Node *cond = or_test(p);
        P_expect(p, "else", "in the conditional expression");
        Node *n = mk(N_Ternary, it, NULL);
        node_add(n, cond);
        node_add(n, c);
        node_add(n, test(p));
        return n;
    }
    if (P_is(p, ":=")) { p->p++; node_free(test(p)); node_free(c); return unsupported(t, "assignment expression (:=)"); }
    return c;
}

/* test (',' test)* -- a Tuple when there is a comma.
 * TL_TARGET parses assignment / loop targets: no comparisons, so the 'in'
 * of "for x in xs" is not read as the membership operator. */
static Node *tl_item(Parser *p, int mode) {
    if (mode != TL_VALUE && P_is(p, "*")) {
        Token *s = P_cur(p);
        p->p++;
        Node *st = mk(N_Star, s, NULL);
        node_add(st, mode == TL_TARGET ? binary(p, 1) : test(p));
        return st;
    }
    return mode == TL_TARGET ? binary(p, 1) : test(p);
}

static Node *testlist(Parser *p, int mode) {
    Token *t = P_cur(p);
    Node *first = tl_item(p, mode);
    if (!P_is(p, ",")) return first;
    Node *tup = mk(N_Tuple, t, NULL);
    node_add(tup, first);
    while (P_accept(p, ",")) {
        if (at_newline(p) || P_is(p, "=") || P_is(p, ")") || P_is(p, "in") || P_is(p, ":") || P_is(p, ";")) break;
        node_add(tup, tl_item(p, mode));
    }
    return tup;
}

/* ---- statements ---- */

static int is_aug(Token *t) {
    static const char *ops[] = { "+=", "-=", "*=", "/=", "//=", "%=", "**=", "&=", "|=", "^=", "<<=", ">>=", "@=", NULL };
    if (t->kind != TK_OP) return 0;
    for (int i = 0; ops[i]; i++) if (strcmp(ops[i], t->text) == 0) return 1;
    return 0;
}

static Node *expr_stmt(Parser *p) {
    Token *t = P_cur(p);
    Node *lhs = testlist(p, TL_VALUE);
    if (P_is(p, ":") ) {                                       /* annotated assignment x: int = 0 */
        p->p++;
        Node *ann = test(p);
        Node *a = mk(N_Assign, t, intern("="));
        a->type = ann->kind == N_Name ? ann->s : NULL;
        node_free(ann);
        node_add(a, lhs);
        if (P_accept(p, "=")) node_add(a, testlist(p, TL_STAR));
        else { a->kind = N_Pass; node_free(lhs); a->k.n = 0; }
        return a;
    }
    if (is_aug(P_cur(p))) {
        Token *op = P_cur(p);
        p->p++;
        Node *a = mk(N_Assign, t, op->text);
        node_add(a, lhs);
        node_add(a, testlist(p, TL_STAR));
        return a;
    }
    if (P_is(p, "=")) {
        /* a = b = value: Assign(=) with every target, value last */
        Node *a = mk(N_Assign, t, intern("="));
        node_add(a, lhs);
        while (P_accept(p, "=")) node_add(a, testlist(p, TL_STAR));
        Node *bad = a->k.v[0];
        if (bad->kind != N_Name && bad->kind != N_Index && bad->kind != N_Member && bad->kind != N_Tuple && bad->kind != N_ArrayInit)
            P_error(p, t, "Syntax Error", "Assign to a name, an element or an attribute.", "cannot assign to this expression");
        return a;
    }
    Node *s = mk(N_ExprStmt, t, NULL);
    node_add(s, lhs);
    return s;
}

static Node *small_stmt(Parser *p) {
    Token *t = P_cur(p);
    if (P_is(p, "pass")) { p->p++; return mk(N_Pass, t, NULL); }
    if (P_is(p, "break")) { p->p++; return mk(N_Break, t, NULL); }
    if (P_is(p, "continue")) { p->p++; return mk(N_Continue, t, NULL); }
    if (P_is(p, "return")) {
        p->p++;
        Node *r = mk(N_Return, t, NULL);
        if (!at_newline(p) && !P_is(p, ";")) node_add(r, testlist(p, TL_STAR));
        return r;
    }
    if (P_is(p, "import") || P_is(p, "from")) {
        int start = t->pos;
        while (!at_newline(p) && !P_is(p, ";")) {
            if (P_is(p, "(")) { while (!P_is(p, ")") && P_cur(p)->kind != TK_EOF) p->p++; }
            p->p++;
        }
        Token *last = &p->t->v[p->p - 1];
        Node *n = mk(N_Import, t, intern_n(p->src + start, (size_t)(last->pos + last->len - start)));
        P_trace(p, "line %d: %s", t->line, n->s);
        return n;
    }
    if (P_is(p, "global") || P_is(p, "nonlocal")) {
        p->p++;
        Node *blk = mk(N_Block, t, NULL);
        do {
            Token *n = P_cur(p);
            if (!is_name(n)) { P_error(p, n, "Syntax Error", NULL, "expected a name"); break; }
            p->p++;
            Node *g = mk(N_Global, n, n->text);
            g->type = intern(t->text);
            node_add(blk, g);
        } while (P_accept(p, ","));
        return blk;
    }
    if (P_is(p, "del")) { p->p++; node_free(testlist(p, TL_VALUE)); return unsupported(t, "del statement"); }
    if (P_is(p, "assert")) {
        p->p++;
        Node *a = mk(N_Assert, t, NULL);
        node_add(a, test(p));
        if (P_accept(p, ",")) node_add(a, test(p));
        return a;
    }
    if (P_is(p, "raise")) {
        p->p++;
        Node *r = mk(N_Throw, t, NULL);
        if (!at_newline(p)) { node_add(r, test(p)); if (P_accept(p, "from")) node_free(test(p)); }
        return r;
    }
    if (P_is(p, "yield")) { while (!at_newline(p) && !P_is(p, ";")) p->p++; return unsupported(t, "yield (generator)"); }
    return expr_stmt(p);
}

static Node *simple_stmts(Parser *p) {
    Node *first = small_stmt(p);
    if (!P_is(p, ";")) {
        if (!at_newline(p) && !p->panic)
            P_error(p, P_cur(p), "Syntax Error", "Put each statement on its own line, or separate them with ';'.",
                    "unexpected '%s' after the statement", P_cur(p)->text);
        if (P_cur(p)->kind == TK_NEWLINE) p->p++;
        return first;
    }
    Node *b = node_new(N_Block, first->line, first->col);
    node_add(b, first);
    while (P_accept(p, ";") && !at_newline(p)) node_add(b, small_stmt(p));
    if (P_cur(p)->kind == TK_NEWLINE) p->p++;
    return b;
}

/* ':' then either an indented block or statements on the same line */
static Node *block_suite(Parser *p) {
    P_expect(p, ":", "to start the block");
    Node *b = P_node(p, N_Block);
    if (p->panic) return b;
    if (P_cur(p)->kind != TK_NEWLINE) { node_add(b, simple_stmts(p)); return b; }
    p->p++;
    if (P_cur(p)->kind != TK_INDENT) {
        P_error(p, P_cur(p), "Indentation Error", "Indent the body of the block.", "expected an indented block");
        return b;
    }
    p->p++;
    while (P_cur(p)->kind != TK_DEDENT && P_cur(p)->kind != TK_EOF) {
        int before = p->p;
        if (P_cur(p)->kind == TK_NEWLINE) { p->p++; continue; }
        Node *s = statement(p);
        if (s) node_add(b, s);
        if (p->panic) sync_line(p);
        if (p->p == before) p->p++;
    }
    if (P_cur(p)->kind == TK_DEDENT) p->p++;
    return b;
}

static Node *params(Parser *p) {
    Node *ps = P_node(p, N_Params);
    P_expect(p, "(", "after the function name");
    while (!P_is(p, ")") && !at_newline(p) && !p->panic) {
        Token *t = P_cur(p);
        if (P_is(p, "/") || (P_is(p, "*") && P_is_at(p, 1, ","))) { p->p++; if (!P_accept(p, ",")) break; continue; }
        int star = P_accept(p, "*") ? 1 : P_accept(p, "**") ? 2 : 0;
        Token *n = P_cur(p);
        if (!is_name(n)) { P_error(p, n, "Syntax Error", NULL, "expected a parameter name, found '%s'", n->text); break; }
        p->p++;
        Node *prm = mk(N_Param, n, n->text);
        if (star) prm->type = intern(star == 1 ? "*args" : "**kwargs");
        if (P_accept(p, ":")) node_free(test(p));               /* annotation */
        if (P_accept(p, "=")) node_add(prm, test(p));
        node_add(ps, prm);
        (void)t;
        if (!P_accept(p, ",")) break;
    }
    P_expect(p, ")", "to close the parameter list");
    if (P_accept(p, "->")) node_free(test(p));
    return ps;
}

static Node *statement(Parser *p) {
    if (!P_enter(p)) { P_leave(p); sync_line(p); return NULL; }
    Token *t = P_cur(p);
    Node *n;
    if (P_is(p, "if")) {
        p->p++;
        n = mk(N_If, t, NULL);
        node_add(n, test(p));
        node_add(n, block_suite(p));
        Node *tail = n;
        while (P_is(p, "elif")) {                         /* elif chains nest as else { if } */
            Token *e = P_cur(p);
            p->p++;
            Node *inner = mk(N_If, e, NULL);
            node_add(inner, test(p));
            node_add(inner, block_suite(p));
            node_add(tail, inner);
            tail = inner;
        }
        if (P_is(p, "else")) { p->p++; node_add(tail, block_suite(p)); }
        P_trace(p, "line %d: if statement", t->line);
    } else if (P_is(p, "while")) {
        p->p++;
        n = mk(N_While, t, NULL);
        node_add(n, test(p));
        node_add(n, block_suite(p));
        if (P_is(p, "else")) { p->p++; node_add(n, block_suite(p)); }
        P_trace(p, "line %d: while loop", t->line);
    } else if (P_is(p, "for")) {
        p->p++;
        n = mk(N_ForEach, t, NULL);
        Node *target = testlist(p, TL_TARGET);
        P_expect(p, "in", "in the for loop");
        Node *iter = testlist(p, TL_STAR);
        if (target->kind == N_Name) { n->s = target->s; node_free(target); }
        else { n->ival = 1; n->s = NULL; }
        node_add(n, iter);
        node_add(n, block_suite(p));
        if (P_is(p, "else")) { p->p++; node_add(n, block_suite(p)); }
        else node_add(n, mk(N_Empty, t, NULL));
        if (n->ival) node_add(n, target);                  /* tuple target kept as the last child */
        P_trace(p, "line %d: for loop", t->line);
    } else if (P_is(p, "def") || (P_is(p, "async") && P_is_at(p, 1, "def"))) {
        int async = P_accept(p, "async");
        p->p++;
        Token *name = P_cur(p);
        if (!is_name(name)) { P_error(p, name, "Syntax Error", NULL, "expected a function name"); P_leave(p); return NULL; }
        p->p++;
        n = mk(N_Function, name, name->text);
        node_add(n, params(p));
        node_add(n, block_suite(p));
        if (async) { node_free(n); n = unsupported(t, "async function"); }
        else P_trace(p, "line %d: function %s() with %d parameter%s", name->line, name->text, n->k.v[0]->k.n, n->k.v[0]->k.n == 1 ? "" : "s");
    } else if (P_is(p, "class")) {
        p->p++;
        Token *name = P_cur(p);
        if (!is_name(name)) { P_error(p, name, "Syntax Error", NULL, "expected a class name"); P_leave(p); return NULL; }
        p->p++;
        n = mk(N_Class, name, name->text);
        if (P_is(p, "(")) { while (!P_is(p, ")") && !at_newline(p)) p->p++; P_accept(p, ")"); }
        Node *body = block_suite(p);
        for (int i = 0; i < body->k.n; i++) node_add(n, body->k.v[i]);
        body->k.n = 0; node_free(body);
        P_trace(p, "line %d: class %s with %d members", name->line, name->text, n->k.n);
    } else if (P_is(p, "try")) {
        p->p++;
        n = mk(N_Try, t, NULL);
        node_add(n, block_suite(p));
        while (P_is(p, "except")) {
            p->p++;
            while (!P_is(p, ":") && !at_newline(p)) p->p++;
            node_add(n, block_suite(p));
        }
        if (P_is(p, "else")) { p->p++; node_add(n, block_suite(p)); }
        if (P_is(p, "finally")) { p->p++; node_add(n, block_suite(p)); }
    } else if (P_is(p, "with") || (P_is(p, "async") && P_is_at(p, 1, "with"))) {
        P_accept(p, "async");
        p->p++;
        while (!P_is(p, ":") && !at_newline(p)) p->p++;
        n = unsupported(t, "with statement");
        node_free(block_suite(p));
    } else if (P_is(p, "@")) {
        p->p++;
        node_free(test(p));
        if (P_cur(p)->kind == TK_NEWLINE) p->p++;
        diag(p->ds, SEV_WARNING, "parser", "Decorator Ignored", t->line, t->col, NULL,
             "decorators are parsed but not applied by the translator");
        n = statement(p);
    } else if (P_is(p, "match") && !P_is_at(p, 1, "=") && !P_is_at(p, 1, "(") && !P_is_at(p, 1, ".")) {
        while (!at_newline(p)) p->p++;
        sync_line(p);
        n = unsupported(t, "match statement");
    } else {
        n = simple_stmts(p);
    }
    P_leave(p);
    return n;
}

Node *parse_python(Parser *p) {
    Node *prog = node_new(N_Program, 1, 1);
    while (P_cur(p)->kind != TK_EOF) {
        int before = p->p;
        if (P_cur(p)->kind == TK_NEWLINE) { p->p++; continue; }
        if (P_cur(p)->kind == TK_INDENT || P_cur(p)->kind == TK_DEDENT) {
            if (P_cur(p)->kind == TK_INDENT)
                P_error(p, P_cur(p), "Indentation Error", "Remove the extra indentation.", "unexpected indent");
            p->p++;
            p->panic = 0;
            continue;
        }
        Node *s = statement(p);
        if (s) node_add(prog, s);
        if (p->panic) sync_line(p);
        if (p->p == before) p->p++;
    }
    return prog;
}
