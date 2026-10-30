/* C and Java share an expression grammar and most statements; they differ
 * in declarations, types and top-level structure. Precedence climbing:
 *
 *   expr     := assign (',' assign)*            (C comma operator)
 *   assign   := ternary (ASSIGN_OP assign)?      right-associative
 *   ternary  := binary(1) ('?' expr ':' ternary)?
 *   binary(k):= binary(k+1) (OP_k binary(k+1))*  left-associative, 10 levels
 *   unary    := ('!'|'~'|'-'|'+'|'++'|'--'|'*'|'&'|sizeof|cast) unary | postfix
 *   postfix  := primary ('[' expr ']' | '(' args ')' | '.' name | '->' name | '++' | '--')*
 */
#include "parse.h"
#include <ctype.h>

#define LANG_IS_JAVA(p) ((p)->lang == LANG_JAVA)

static Node *expr(Parser *p);
static Node *assign_expr(Parser *p);
static Node *statement(Parser *p);
static Node *block(Parser *p);
static Node *unary(Parser *p);

/* ---- error recovery ---- */

/* Skips to just after the next ';' or to (not past) a '}' that closes the
 * current block, keeping bracket nesting balanced on the way. */
static void sync_statement(Parser *p) {
    int depth = 0;
    while (P_cur(p)->kind != TK_EOF) {
        Token *t = P_cur(p);
        if (depth == 0 && strcmp(t->text, ";") == 0) { p->p++; break; }
        if (depth == 0 && strcmp(t->text, "}") == 0) break;
        if (strcmp(t->text, "{") == 0 || strcmp(t->text, "(") == 0 || strcmp(t->text, "[") == 0) depth++;
        if (strcmp(t->text, "}") == 0 || strcmp(t->text, ")") == 0 || strcmp(t->text, "]") == 0) {
            depth--;
            if (depth == 0 && strcmp(t->text, "}") == 0) { p->p++; break; }
        }
        p->p++;
    }
    p->panic = 0;
}

/* Skips a balanced (...) / {...} / <...> group starting at the current token. */
static void skip_balanced(Parser *p, const char *open, const char *close) {
    int depth = 0;
    do {
        if (P_is(p, open)) depth++;
        else if (P_is(p, close)) depth--;
        else if (strcmp(open, "<") == 0 && P_is(p, ">>")) depth -= 2;
        else if (strcmp(open, "<") == 0 && P_is(p, ">>>")) depth -= 3;
        if (P_cur(p)->kind == TK_EOF) return;
        p->p++;
    } while (depth > 0);
}

static Node *unsupported(Parser *p, Token *at, const char *what) {
    Node *n = node_new(N_Unsupported, at->line, at->col);
    n->s = intern(what);
    return n;
}

/* ---- types ---- */

static const char *c_type_words[] = {
    "void", "char", "short", "int", "long", "float", "double", "signed", "unsigned", "_Bool",
    "const", "volatile", "static", "extern", "register", "inline", "auto", "restrict",
    "struct", "union", "enum", "_Complex", "_Atomic", "_Noreturn", NULL };
static const char *c_std_typedefs[] = {
    "size_t", "ssize_t", "FILE", "bool", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t",
    "int16_t", "int32_t", "int64_t", "uintptr_t", "intptr_t", "ptrdiff_t", "wchar_t", "time_t",
    "clock_t", "va_list", NULL };
static const char *java_prims[] = { "int", "long", "short", "byte", "char", "boolean", "float", "double", "void", "var", NULL };
static const char *java_mods[] = {
    "public", "private", "protected", "static", "final", "abstract", "native", "synchronized",
    "transient", "volatile", "strictfp", "default", "sealed", NULL };

static int in_list(const char *s, const char **list) {
    for (int i = 0; list[i]; i++) if (strcmp(s, list[i]) == 0) return 1;
    return 0;
}

static int is_typedef_name(Parser *p, const char *s) {
    if (in_list(s, c_std_typedefs)) return 1;
    for (int i = 0; i < p->typedefs.n; i++) if (p->typedefs.v[i] == s) return 1;
    return 0;
}

static int c_type_start(Parser *p, int ahead) {
    Token *t = P_peek(p, ahead);
    if (t->kind == TK_KEYWORD && in_list(t->text, c_type_words)) return 1;
    return t->kind == TK_IDENT && is_typedef_name(p, t->text);
}

/* C declaration specifiers: "const unsigned long", "struct Node", "size_t".
 * Sets NF_STATIC / NF_UNSIGNED / NF_CONST in *flags. */
static const char *c_specifiers(Parser *p, int *flags) {
    Buf b = {0};
    int saw_type = 0;
    for (;;) {
        Token *t = P_cur(p);
        if (t->kind == TK_KEYWORD && (strcmp(t->text, "struct") == 0 || strcmp(t->text, "union") == 0 || strcmp(t->text, "enum") == 0)) {
            if (b.n) bputc(&b, ' ');
            bputs(&b, t->text);
            p->p++;
            if (P_cur(p)->kind == TK_IDENT) { bputc(&b, ' '); bputs(&b, P_cur(p)->text); p->p++; }
            saw_type = 1;
            if (P_is(p, "{")) break;             /* definition body: caller handles */
            continue;
        }
        if (t->kind == TK_KEYWORD && in_list(t->text, c_type_words)) {
            if (strcmp(t->text, "static") == 0) *flags |= NF_STATIC;
            else if (strcmp(t->text, "unsigned") == 0) *flags |= NF_UNSIGNED;
            else if (strcmp(t->text, "const") == 0) *flags |= NF_CONST;
            if (strcmp(t->text, "static") && strcmp(t->text, "extern") && strcmp(t->text, "register") &&
                strcmp(t->text, "inline") && strcmp(t->text, "auto") && strcmp(t->text, "restrict") &&
                strcmp(t->text, "volatile") && strcmp(t->text, "const") && strcmp(t->text, "_Noreturn")) {
                if (b.n) bputc(&b, ' ');
                bputs(&b, t->text);
                saw_type = 1;
            }
            p->p++;
            continue;
        }
        if (!saw_type && t->kind == TK_IDENT && is_typedef_name(p, t->text)) {
            if (b.n) bputc(&b, ' ');
            bputs(&b, t->text);
            saw_type = 1;
            p->p++;
            continue;
        }
        break;
    }
    const char *r = intern(b.n ? b.s : "int");
    free(b.s);
    return r;
}

/* Skips Java type arguments <...>. Fails (returns 0) on any token that
 * cannot appear inside them, so "a < b" in an expression is never taken
 * for a generic type. */
static int skip_type_args(Parser *p) {
    int depth = 0;
    do {
        Token *t = P_cur(p);
        if (P_is(p, "<")) depth++;
        else if (P_is(p, ">")) depth--;
        else if (P_is(p, ">>")) depth -= 2;
        else if (P_is(p, ">>>")) depth -= 3;
        else if (!(t->kind == TK_IDENT || P_is(p, ",") || P_is(p, ".") || P_is(p, "?") || P_is(p, "[") ||
                   P_is(p, "]") || P_is(p, "&") || P_is(p, "extends") || P_is(p, "super") ||
                   (t->kind == TK_KEYWORD && in_list(t->text, java_prims)))) return 0;
        p->p++;
    } while (depth > 0);
    return depth == 0;
}

/* Java type: primitive | Name(.Name)* <args>? followed by []*.  Returns NULL
 * (and restores the position) if no type is here. */
static const char *java_type(Parser *p) {
    int save = p->p;
    Buf b = {0};
    Token *t = P_cur(p);
    if (t->kind == TK_KEYWORD && in_list(t->text, java_prims)) { bputs(&b, t->text); p->p++; }
    else if (t->kind == TK_IDENT) {
        bputs(&b, t->text); p->p++;
        for (;;) {
            if (P_is(p, "<")) {
                int start = P_cur(p)->pos;
                if (!skip_type_args(p)) { p->p = save; free(b.s); return NULL; }
                Token *last = &p->t->v[p->p - 1];
                bput(&b, p->src + start, (size_t)(last->pos + last->len - start));
                continue;
            }
            if (P_is(p, ".") && P_peek(p, 1)->kind == TK_IDENT) { bputc(&b, '.'); p->p++; bputs(&b, P_cur(p)->text); p->p++; continue; }
            break;
        }
    } else { p->p = save; free(b.s); return NULL; }
    while (P_is(p, "[") && P_is_at(p, 1, "]")) { bputs(&b, "[]"); p->p += 2; }
    if (P_is(p, "...")) { bputs(&b, "[]"); p->p++; }        /* varargs are arrays */
    const char *r = intern(b.s);
    free(b.s);
    return r;
}

static void skip_annotation(Parser *p) {
    p->p++;                                    /* '@' */
    if (P_cur(p)->kind == TK_IDENT || P_cur(p)->kind == TK_KEYWORD) p->p++;
    while (P_is(p, ".") && P_peek(p, 1)->kind == TK_IDENT) p->p += 2;
    if (P_is(p, "(")) skip_balanced(p, "(", ")");
}

static int java_modifiers(Parser *p) {
    int flags = 0;
    for (;;) {
        if (P_is(p, "@") && !P_is_at(p, 1, "interface")) { skip_annotation(p); continue; }
        if (P_cur(p)->kind == TK_KEYWORD && in_list(P_cur(p)->text, java_mods)) {
            if (P_is(p, "static")) flags |= NF_STATIC;
            if (P_is(p, "final")) flags |= NF_CONST;
            p->p++;
            continue;
        }
        if (P_cur(p)->kind == TK_IDENT && (P_is(p, "sealed") || P_is(p, "non"))) { p->p++; continue; }
        break;
    }
    return flags;
}

/* Is a local variable declaration starting here? (Java: Type Name ...) */
static int java_decl_ahead(Parser *p) {
    int save = p->p, panic = p->panic;
    while (P_is(p, "final") || P_is(p, "@")) { if (P_is(p, "@")) skip_annotation(p); else p->p++; }
    const char *ty = java_type(p);
    int ok = 0;
    if (ty && P_cur(p)->kind == TK_IDENT) {
        Token *n = P_peek(p, 1);
        ok = strcmp(n->text, "=") == 0 || strcmp(n->text, ";") == 0 || strcmp(n->text, ",") == 0 ||
             strcmp(n->text, "[") == 0 || strcmp(n->text, ":") == 0;
    }
    p->p = save; p->panic = panic;
    return ok;
}

/* ---- expressions ---- */

static Node *mk(NodeKind k, Token *t, const char *s) { Node *n = node_new(k, t->line, t->col); n->s = s; return n; }

static Node *array_init(Parser *p) {
    Node *n = P_node(p, N_ArrayInit);
    P_expect(p, "{", "to start the initializer");
    while (!P_is(p, "}") && P_cur(p)->kind != TK_EOF && !p->panic) {
        if (P_is(p, "{")) node_add(n, array_init(p));
        else if (P_is(p, ".") || P_is(p, "[")) {          /* C99 designated initializer */
            node_add(n, unsupported(p, P_cur(p), "designated initializer"));
            while (!P_is(p, ",") && !P_is(p, "}") && P_cur(p)->kind != TK_EOF) p->p++;
        }
        else node_add(n, assign_expr(p));
        if (!P_accept(p, ",")) break;
    }
    P_expect(p, "}", "to close the initializer");
    return n;
}

static Node *args(Parser *p, Node *call) {
    P_expect(p, "(", "to start the argument list");
    while (!P_is(p, ")") && P_cur(p)->kind != TK_EOF && !p->panic) {
        node_add(call, assign_expr(p));
        if (!P_accept(p, ",")) break;
    }
    P_expect(p, ")", "to close the argument list");
    return call;
}

static Node *lambda_rest(Parser *p, Token *start) {
    /* the parameters have been skipped; we are at '->' */
    Node *n = mk(N_Lambda, start, NULL);
    p->p++;
    if (P_is(p, "{")) node_add(n, block(p));
    else node_add(n, assign_expr(p));
    return n;
}

static Node *primary(Parser *p) {
    Token *t = P_cur(p);
    switch (t->kind) {
    case TK_NUMBER: {
        p->p++;
        if (is_float_literal(t->text, p->lang)) { Node *n = mk(N_FloatLit, t, t->text); n->fval = parse_float_literal(t->text); return n; }
        int ok;
        Node *n = mk(N_IntLit, t, t->text);
        n->ival = parse_int_literal(t->text, &ok);
        size_t L = strlen(t->text);
        if (L && strchr("lL", t->text[L - 1])) n->type = intern("long");
        if (!ok) diag(p->ds, SEV_WARNING, "parser", "Literal Range", t->line, t->col, NULL, "integer literal %s does not fit in 64 bits", t->text);
        return n;
    }
    case TK_CHAR: { p->p++; Node *n = mk(N_CharLit, t, t->text); n->ival = decode_char(t->text); return n; }
    case TK_STRING: {
        /* C concatenates adjacent string literals */
        Buf b = {0};
        bputs(&b, decode_string(t->text, p->lang, NULL, NULL));
        p->p++;
        while (p->lang == LANG_C && P_cur(p)->kind == TK_STRING) { bputs(&b, decode_string(P_cur(p)->text, p->lang, NULL, NULL)); p->p++; }
        Node *n = mk(N_StrLit, t, intern_n(b.s ? b.s : "", b.n));
        free(b.s);
        return n;
    }
    case TK_IDENT: {
        if (LANG_IS_JAVA(p) && P_is_at(p, 1, "->")) { p->p++; return lambda_rest(p, t); }
        p->p++;
        return mk(N_Name, t, t->text);
    }
    case TK_KEYWORD:
        if (P_is(p, "true") || P_is(p, "false")) { p->p++; Node *n = mk(N_BoolLit, t, t->text); n->ival = t->text[0] == 't'; return n; }
        if (P_is(p, "null")) { p->p++; return mk(N_NullLit, t, t->text); }
        if (P_is(p, "this") || P_is(p, "super")) { p->p++; return mk(N_Name, t, t->text); }
        if (LANG_IS_JAVA(p) && P_is(p, "new")) {
            p->p++;
            Token *tt = P_cur(p);
            const char *base;
            if (tt->kind == TK_KEYWORD && in_list(tt->text, java_prims)) { base = tt->text; p->p++; }
            else {
                Buf b = {0};
                if (tt->kind == TK_IDENT) { bputs(&b, tt->text); p->p++; }
                else P_error(p, tt, "Syntax Error", NULL, "expected a type after 'new'");
                while (P_is(p, ".") && P_peek(p, 1)->kind == TK_IDENT) { bputc(&b, '.'); p->p++; bputs(&b, P_cur(p)->text); p->p++; }
                base = intern(b.s ? b.s : "?");
                free(b.s);
                if (P_is(p, "<")) skip_balanced(p, "<", ">");
            }
            if (P_is(p, "[")) {
                Node *n = mk(N_NewArray, t, NULL);
                n->type = base;
                int rank = 0;
                while (P_accept(p, "[")) {
                    if (P_is(p, "]")) node_add(n, mk(N_Empty, P_cur(p), NULL));
                    else node_add(n, expr(p));
                    P_expect(p, "]", "after the array size");
                    rank++;
                }
                n->ival = rank;
                if (P_is(p, "{")) { n->k.n = 0; node_add(n, array_init(p)); }
                return n;
            }
            Node *n = mk(N_NewObject, t, base);
            args(p, n);
            if (P_is(p, "{")) {                                    /* anonymous class */
                skip_balanced(p, "{", "}");
                Node *u = unsupported(p, t, "anonymous class");
                node_free(n);
                return u;
            }
            return n;
        }
        if (p->lang == LANG_C && P_is(p, "sizeof")) {
            p->p++;
            Node *n = mk(N_Sizeof, t, NULL);
            if (P_is(p, "(") && c_type_start(p, 1)) {
                p->p++;
                int f = 0;
                Buf b = {0};
                bputs(&b, c_specifiers(p, &f));
                while (P_accept(p, "*")) bputc(&b, '*');
                n->type = intern(b.s);
                free(b.s);
                P_expect(p, ")", "after the type in sizeof");
            } else node_add(n, unary(p));
            return n;
        }
        if (LANG_IS_JAVA(p) && in_list(t->text, java_prims) && P_is_at(p, 1, ".") && P_is_at(p, 2, "class")) {
            p->p += 3;
            return unsupported(p, t, "class literal");
        }
        break;
    case TK_PUNCT:
        if (P_is(p, "(")) {
            /* Java lambda with a parameter list: (a, b) -> ... */
            if (LANG_IS_JAVA(p)) {
                int save = p->p;
                skip_balanced(p, "(", ")");
                if (P_is(p, "->")) return lambda_rest(p, t);
                p->p = save;
            }
            p->p++;
            Node *e = expr(p);
            P_expect(p, ")", "to close the parenthesis");
            return e;
        }
        if (P_is(p, "{") && p->lang == LANG_C) return array_init(p);
        break;
    default:
        break;
    }
    P_error(p, t, "Syntax Error", "Check for a missing operand or an extra operator.",
            "expected an expression, found %s", t->kind == TK_EOF ? "end of file" : t->text);
    if (t->kind != TK_EOF && strcmp(t->text, ";") && strcmp(t->text, "}") && strcmp(t->text, ")")) p->p++;
    return mk(N_Unsupported, t, intern("syntax error"));
}

static Node *postfix(Parser *p, Node *e) {
    for (;;) {
        Token *t = P_cur(p);
        if (P_is(p, "[")) {
            p->p++;
            Node *n = mk(N_Index, t, NULL);
            node_add(n, e);
            node_add(n, expr(p));
            P_expect(p, "]", "to close the index");
            e = n;
        } else if (P_is(p, "(")) {
            Node *n = mk(N_Call, t, NULL);
            n->line = e->line; n->col = e->col;
            node_add(n, e);
            e = args(p, n);
        } else if (P_is(p, ".") || P_is(p, "->")) {
            int arrow = P_is(p, "->");
            p->p++;
            Token *f = P_cur(p);
            if (LANG_IS_JAVA(p) && P_is(p, "<")) skip_balanced(p, "<", ">");   /* obj.<T>method() */
            f = P_cur(p);
            if (f->kind != TK_IDENT && f->kind != TK_KEYWORD) { P_error(p, f, "Syntax Error", NULL, "expected a member name after '%s'", arrow ? "->" : "."); return e; }
            p->p++;
            Node *n = mk(N_Member, f, f->text);
            n->ival = arrow;
            node_add(n, e);
            e = n;
        } else if (LANG_IS_JAVA(p) && P_is(p, "::")) {
            p->p++;
            if (P_cur(p)->kind != TK_EOF) p->p++;
            Node *u = unsupported(p, t, "method reference");
            node_free(e);
            e = u;
        } else if (P_is(p, "++") || P_is(p, "--")) {
            p->p++;
            Node *n = mk(N_IncDec, t, t->text);
            n->ival = 0;
            node_add(n, e);
            e = n;
        } else return e;
    }
}

/* Is "( type )" a cast here? */
static int cast_ahead(Parser *p) {
    if (!P_is(p, "(")) return 0;
    if (p->lang == LANG_C) return c_type_start(p, 1);
    Token *t = P_peek(p, 1);
    if (t->kind == TK_KEYWORD && in_list(t->text, java_prims) && strcmp(t->text, "var")) {
        int save = p->p; p->p++;
        java_type(p);
        int ok = P_is(p, ")");
        p->p = save;
        return ok;
    }
    if (t->kind != TK_IDENT) return 0;
    /* (Name) x  -- a reference cast only if what follows can start an operand */
    int save = p->p, panic = p->panic;
    p->p++;
    const char *ty = java_type(p);
    int ok = 0;
    if (ty && P_is(p, ")")) {
        Token *n = P_peek(p, 1);
        ok = n->kind == TK_IDENT || n->kind == TK_STRING || n->kind == TK_NUMBER || n->kind == TK_CHAR ||
             strcmp(n->text, "(") == 0 || strcmp(n->text, "!") == 0 || strcmp(n->text, "~") == 0 ||
             strcmp(n->text, "this") == 0 || strcmp(n->text, "new") == 0;
        if (n->kind == TK_KEYWORD && (strcmp(n->text, "instanceof") == 0)) ok = 0;
    }
    p->p = save; p->panic = panic;
    return ok;
}

static Node *unary(Parser *p) {
    if (!P_enter(p)) { P_leave(p); return mk(N_Unsupported, P_cur(p), intern("too deep")); }
    Token *t = P_cur(p);
    Node *r;
    if (P_is(p, "!") || P_is(p, "~") || P_is(p, "-") || P_is(p, "+") ||
        (p->lang == LANG_C && (P_is(p, "*") || P_is(p, "&")))) {
        p->p++;
        r = mk(N_Unary, t, t->text);
        node_add(r, unary(p));
    } else if (P_is(p, "++") || P_is(p, "--")) {
        p->p++;
        r = mk(N_IncDec, t, t->text);
        r->ival = 1;
        node_add(r, unary(p));
    } else if (cast_ahead(p)) {
        p->p++;
        r = mk(N_Cast, t, NULL);
        if (p->lang == LANG_C) {
            int f = 0;
            Buf b = {0};
            bputs(&b, c_specifiers(p, &f));
            while (P_accept(p, "*")) bputc(&b, '*');
            r->type = intern(b.s);
            free(b.s);
        } else r->type = java_type(p);
        P_expect(p, ")", "to close the cast");
        node_add(r, unary(p));
    } else {
        r = postfix(p, primary(p));
    }
    P_leave(p);
    return r;
}

static int binary_prec(Parser *p, const char *op) {
    static const struct { const char *op; int prec; } table[] = {
        {"||", 1}, {"&&", 2}, {"|", 3}, {"^", 4}, {"&", 5}, {"==", 6}, {"!=", 6},
        {"<", 7}, {">", 7}, {"<=", 7}, {">=", 7}, {"instanceof", 7},
        {"<<", 8}, {">>", 8}, {">>>", 8}, {"+", 9}, {"-", 9}, {"*", 10}, {"/", 10}, {"%", 10},
    };
    if (P_cur(p)->kind == TK_STRING || P_cur(p)->kind == TK_CHAR) return 0;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) if (strcmp(table[i].op, op) == 0) return table[i].prec;
    return 0;
}

static Node *binary(Parser *p, int min_prec) {
    Node *lhs = unary(p);
    for (;;) {
        Token *t = P_cur(p);
        int prec = binary_prec(p, t->text);
        if (!prec || prec < min_prec) return lhs;
        p->p++;
        if (strcmp(t->text, "instanceof") == 0) {
            java_type(p);
            if (P_cur(p)->kind == TK_IDENT) p->p++;            /* pattern variable */
            Node *u = unsupported(p, t, "instanceof");
            node_free(lhs);
            lhs = u;
            continue;
        }
        Node *rhs = binary(p, prec + 1);
        Node *n = mk(prec <= 2 ? N_Logical : N_Binary, t, t->text);
        node_add(n, lhs);
        node_add(n, rhs);
        lhs = n;
    }
}

static Node *ternary(Parser *p) {
    Node *c = binary(p, 1);
    if (!P_is(p, "?")) return c;
    Token *t = P_cur(p);
    p->p++;
    Node *n = mk(N_Ternary, t, NULL);
    node_add(n, c);
    node_add(n, expr(p));
    P_expect(p, ":", "in the conditional expression");
    node_add(n, ternary(p));
    return n;
}

static int is_assign_op(const char *s) {
    static const char *ops[] = { "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", ">>>=", NULL };
    return in_list(s, ops);
}

static Node *assign_expr(Parser *p) {
    Node *lhs = ternary(p);
    Token *t = P_cur(p);
    if (t->kind == TK_OP && is_assign_op(t->text)) {
        p->p++;
        if (lhs->kind != N_Name && lhs->kind != N_Index && lhs->kind != N_Member &&
            !(lhs->kind == N_Unary && strcmp(lhs->s, "*") == 0))
            P_error(p, t, "Syntax Error", "Assign to a variable, an array element or a field.",
                    "the left side of '%s' cannot be assigned to", t->text);
        Node *n = mk(N_Assign, t, t->text);
        n->line = lhs->line; n->col = lhs->col;
        node_add(n, lhs);
        node_add(n, assign_expr(p));
        return n;
    }
    return lhs;
}

static Node *expr(Parser *p) {
    Node *e = assign_expr(p);
    while (p->lang == LANG_C && P_is(p, ",")) {
        Token *t = P_cur(p);
        p->p++;
        Node *n = mk(N_Binary, t, intern(","));
        node_add(n, e);
        node_add(n, assign_expr(p));
        e = n;
    }
    return e;
}

/* Java allows ',' in for-init/update lists; collect them as a Block of ExprStmts. */
static Node *expr_list(Parser *p) {
    Node *first = expr(p);
    if (!P_is(p, ",")) return first;
    Node *b = node_new(N_Block, first->line, first->col);
    Node *s = node_new(N_ExprStmt, first->line, first->col); node_add(s, first); node_add(b, s);
    while (P_accept(p, ",")) {
        Node *e = expr(p);
        s = node_new(N_ExprStmt, e->line, e->col); node_add(s, e); node_add(b, s);
    }
    return b;
}

/* ---- declarations ---- */

/* One declarator after the specifiers: pointers, name, array dims, init.
 * VarDecl children: [init or Empty, dim1, dim2, ...] */
static Node *declarator(Parser *p, const char *base, int flags) {
    Buf ty = {0};
    bputs(&ty, base);
    while (p->lang == LANG_C && (P_is(p, "*") || P_is(p, "const") || P_is(p, "restrict"))) {
        if (P_is(p, "*")) { bputc(&ty, '*'); flags |= NF_POINTER; }
        p->p++;
    }
    Token *nt = P_cur(p);
    if (p->lang == LANG_C && P_is(p, "(") && P_is_at(p, 1, "*")) {      /* function pointer */
        Node *u = unsupported(p, nt, "function pointer declaration");
        free(ty.s);
        while (!P_is(p, ";") && !P_is(p, ",") && P_cur(p)->kind != TK_EOF) {
            if (P_is(p, "(")) skip_balanced(p, "(", ")"); else p->p++;
        }
        return u;
    }
    Node *d = mk(N_VarDecl, nt, NULL);
    if (nt->kind != TK_IDENT) {
        P_error(p, nt, "Syntax Error", "Give the variable a name.", "expected a variable name, found '%s'", nt->text);
        free(ty.s);
        d->type = intern(base);
        return d;
    }
    d->s = nt->text;
    p->p++;
    Node *init = mk(N_Empty, nt, NULL);
    node_add(d, init);
    int rank = 0;
    while (P_accept(p, "[")) {
        if (P_is(p, "]")) node_add(d, mk(N_Empty, P_cur(p), NULL));
        else node_add(d, expr(p));
        P_expect(p, "]", "after the array size");
        rank++;
        if (p->lang == LANG_JAVA) bputs(&ty, "[]");
    }
    if (p->lang == LANG_C) for (int i = 0; i < rank; i++) bputs(&ty, "[]");
    d->ival = rank;
    d->flags = flags;
    d->type = intern(ty.s);
    free(ty.s);
    if (P_accept(p, "=")) {
        node_free(d->k.v[0]);
        d->k.v[0] = P_is(p, "{") ? array_init(p) : assign_expr(p);
    }
    return d;
}

static Node *decl_group(Parser *p, const char *base, int flags, int need_semi) {
    Node *g = P_node(p, N_DeclGroup);
    do {
        node_add(g, declarator(p, base, flags));
    } while (!p->panic && P_accept(p, ","));
    if (need_semi) P_expect(p, ";", "after the declaration");
    P_trace(p, "line %d: declaration of %d variable%s (%s)", g->line, g->k.n, g->k.n == 1 ? "" : "s", base);
    return g;
}

static Node *local_decl(Parser *p, int need_semi) {
    int flags = 0;
    const char *base;
    if (p->lang == LANG_C) base = c_specifiers(p, &flags);
    else {
        while (P_is(p, "final") || P_is(p, "@")) { if (P_is(p, "@")) skip_annotation(p); else { flags |= NF_CONST; p->p++; } }
        base = java_type(p);
    }
    return decl_group(p, base, flags, need_semi);
}

static int decl_ahead(Parser *p) {
    if (p->lang == LANG_C) {
        if (!c_type_start(p, 0)) return 0;
        /* "T (" after a typedef name could be a call; typedef names followed by ident or '*' are decls */
        Token *t = P_cur(p);
        if (t->kind == TK_IDENT) { Token *n = P_peek(p, 1); return n->kind == TK_IDENT || strcmp(n->text, "*") == 0; }
        return 1;
    }
    return java_decl_ahead(p);
}

/* ---- statements ---- */

static Node *block(Parser *p) {
    Node *b = P_node(p, N_Block);
    if (!P_expect(p, "{", "to start a block")) return b;
    while (!P_is(p, "}") && P_cur(p)->kind != TK_EOF) {
        int before = p->p;
        Node *s = statement(p);
        if (s) node_add(b, s);
        if (p->panic) sync_statement(p);
        if (p->p == before) p->p++;              /* always make progress */
    }
    P_expect(p, "}", "to close the block");
    return b;
}

static Node *paren_cond(Parser *p, const char *what) {
    char where[64];
    snprintf(where, sizeof(where), "after '%s'", what);
    P_expect(p, "(", where);
    Node *c = expr(p);
    P_expect(p, ")", "after the condition");
    return c;
}

static Node *switch_stmt(Parser *p, Token *t) {
    Node *n = mk(N_Switch, t, NULL);
    node_add(n, paren_cond(p, "switch"));
    P_expect(p, "{", "to start the switch body");
    Node *cur = NULL;
    while (!P_is(p, "}") && P_cur(p)->kind != TK_EOF) {
        Token *c = P_cur(p);
        if (P_is(p, "case") || P_is(p, "default")) {
            p->p++;
            cur = mk(N_Case, c, strcmp(c->text, "default") == 0 ? intern("default") : NULL);
            if (!cur->s) {
                node_add(cur, ternary(p));
                while (P_accept(p, ",")) {             /* Java: case 1, 2 -> */
                    Node *extra = mk(N_Case, c, NULL);
                    node_add(extra, cur->k.v[0]);
                    cur->k.v[0] = ternary(p);
                    node_add(n, extra);                  /* empty case falls through to cur */
                }
            } else {
                Node *e = mk(N_Empty, c, NULL);
                node_add(cur, e);
            }
            node_add(n, cur);
            if (LANG_IS_JAVA(p) && P_accept(p, "->")) {   /* arrow form: no fallthrough */
                Node *body = P_is(p, "{") ? block(p) : statement(p);
                node_add(cur, body);
                node_add(cur, mk(N_Break, c, NULL));
                cur = NULL;
                continue;
            }
            P_expect(p, ":", "after the case label");
            continue;
        }
        if (!cur) { P_error(p, c, "Syntax Error", "Start with 'case' or 'default'.", "statement before the first case label"); sync_statement(p); continue; }
        int before = p->p;
        Node *s = statement(p);
        if (s) node_add(cur, s);
        if (p->panic) sync_statement(p);
        if (p->p == before) p->p++;
    }
    P_expect(p, "}", "to close the switch");
    P_trace(p, "line %d: switch with %d case labels", t->line, n->k.n - 1);
    return n;
}

static Node *statement(Parser *p) {
    if (!P_enter(p)) { P_leave(p); return NULL; }
    Token *t = P_cur(p);
    Node *n = NULL;
    if (P_is(p, "{")) n = block(p);
    else if (P_is(p, ";")) { p->p++; n = mk(N_Empty, t, NULL); }
    else if (P_is(p, "if")) {
        p->p++;
        n = mk(N_If, t, NULL);
        node_add(n, paren_cond(p, "if"));
        node_add(n, statement(p));
        if (P_accept(p, "else")) node_add(n, statement(p));
        P_trace(p, "line %d: if%s statement", t->line, n->k.n == 3 ? "/else" : "");
    } else if (P_is(p, "while")) {
        p->p++;
        n = mk(N_While, t, NULL);
        node_add(n, paren_cond(p, "while"));
        node_add(n, statement(p));
        P_trace(p, "line %d: while loop", t->line);
    } else if (P_is(p, "do")) {
        p->p++;
        n = mk(N_DoWhile, t, NULL);
        node_add(n, statement(p));
        if (!P_expect(p, "while", "after the do-loop body")) { P_leave(p); return n; }
        node_add(n, paren_cond(p, "while"));
        P_expect(p, ";", "after do ... while (...)");
        P_trace(p, "line %d: do-while loop", t->line);
    } else if (P_is(p, "for")) {
        p->p++;
        P_expect(p, "(", "after 'for'");
        if (LANG_IS_JAVA(p) && java_decl_ahead(p)) {
            int save = p->p;
            while (P_is(p, "final") || P_is(p, "@")) { if (P_is(p, "@")) skip_annotation(p); else p->p++; }
            const char *ty = java_type(p);
            Token *name = P_cur(p);
            if (ty && name->kind == TK_IDENT && P_is_at(p, 1, ":")) {
                p->p += 2;
                n = mk(N_ForEach, t, name->text);
                n->type = ty;
                node_add(n, expr(p));
                P_expect(p, ")", "after the for-each header");
                node_add(n, statement(p));
                P_trace(p, "line %d: for-each loop over '%s'", t->line, name->text);
                P_leave(p);
                return n;
            }
            p->p = save;
        }
        n = mk(N_For, t, NULL);
        if (P_is(p, ";")) { node_add(n, mk(N_Empty, P_cur(p), NULL)); p->p++; }
        else if (decl_ahead(p)) node_add(n, local_decl(p, 1));
        else { node_add(n, expr_list(p)); P_expect(p, ";", "after the for-loop initializer"); }
        if (P_is(p, ";")) node_add(n, mk(N_Empty, P_cur(p), NULL)); else node_add(n, expr(p));
        P_expect(p, ";", "after the for-loop condition");
        if (P_is(p, ")")) node_add(n, mk(N_Empty, P_cur(p), NULL)); else node_add(n, expr_list(p));
        P_expect(p, ")", "after the for-loop header");
        node_add(n, statement(p));
        P_trace(p, "line %d: for loop", t->line);
    } else if (P_is(p, "switch")) { p->p++; n = switch_stmt(p, t); }
    else if (P_is(p, "return")) {
        p->p++;
        n = mk(N_Return, t, NULL);
        if (!P_is(p, ";")) node_add(n, expr(p));
        P_expect(p, ";", "after the return statement");
    } else if (P_is(p, "break") || P_is(p, "continue")) {
        p->p++;
        if (P_cur(p)->kind == TK_IDENT) { p->p++; n = unsupported(p, t, "labelled break/continue"); }
        else n = mk(t->text[0] == 'b' ? N_Break : N_Continue, t, NULL);
        P_expect(p, ";", t->text[0] == 'b' ? "after 'break'" : "after 'continue'");
    } else if (P_is(p, "goto")) {
        p->p++;
        if (P_cur(p)->kind == TK_IDENT) p->p++;
        P_expect(p, ";", "after goto");
        n = unsupported(p, t, "goto");
    } else if (LANG_IS_JAVA(p) && P_is(p, "try")) {
        p->p++;
        n = mk(N_Try, t, NULL);
        if (P_is(p, "(")) skip_balanced(p, "(", ")");      /* try-with-resources */
        node_add(n, block(p));
        while (P_is(p, "catch")) {
            Token *c = P_cur(p);
            p->p++;
            skip_balanced(p, "(", ")");
            Node *h = block(p);
            h->line = c->line;
            node_add(n, h);
        }
        if (P_accept(p, "finally")) node_add(n, block(p));
        P_trace(p, "line %d: try statement (%d handlers)", t->line, n->k.n - 1);
    } else if (LANG_IS_JAVA(p) && P_is(p, "throw")) {
        p->p++;
        n = mk(N_Throw, t, NULL);
        node_add(n, expr(p));
        P_expect(p, ";", "after throw");
    } else if (LANG_IS_JAVA(p) && P_is(p, "assert")) {
        p->p++;
        n = mk(N_Assert, t, NULL);
        node_add(n, expr(p));
        if (P_accept(p, ":")) node_add(n, expr(p));
        P_expect(p, ";", "after assert");
    } else if (LANG_IS_JAVA(p) && P_is(p, "synchronized")) {
        p->p++;
        paren_cond(p, "synchronized");
        n = block(p);
    } else if (LANG_IS_JAVA(p) && (P_is(p, "class") || P_is(p, "interface") || P_is(p, "record") ||
                                   (P_is(p, "enum") && P_peek(p, 1)->kind == TK_IDENT))) {
        while (!P_is(p, "{") && P_cur(p)->kind != TK_EOF) p->p++;
        skip_balanced(p, "{", "}");
        n = unsupported(p, t, "local class");
    } else if (P_cur(p)->kind == TK_IDENT && P_is_at(p, 1, ":") && !P_is_at(p, 2, ":")) {
        p->p += 2;                                          /* label: statement */
        n = statement(p);
    } else if (decl_ahead(p)) {
        n = local_decl(p, 1);
    } else if (p->lang == LANG_C && (P_is(p, "struct") || P_is(p, "union") || P_is(p, "enum") || P_is(p, "typedef"))) {
        sync_statement(p);
        n = unsupported(p, t, "local type declaration");
    } else {
        n = mk(N_ExprStmt, t, NULL);
        node_add(n, expr(p));
        P_expect(p, ";", "after the expression");
    }
    P_leave(p);
    return n;
}

/* ---- C top level ---- */

static Node *c_params(Parser *p) {
    Node *ps = P_node(p, N_Params);
    P_expect(p, "(", "to start the parameter list");
    if (P_is(p, "void") && P_is_at(p, 1, ")")) p->p++;
    while (!P_is(p, ")") && P_cur(p)->kind != TK_EOF && !p->panic) {
        if (P_accept(p, "...")) { Node *v = P_node(p, N_Param); v->s = intern("..."); v->type = intern("..."); node_add(ps, v); break; }
        int flags = 0;
        const char *base = c_specifiers(p, &flags);
        Buf ty = {0};
        bputs(&ty, base);
        while (P_is(p, "*") || P_is(p, "const") || P_is(p, "restrict")) { if (P_is(p, "*")) { bputc(&ty, '*'); flags |= NF_POINTER; } p->p++; }
        Node *prm = P_node(p, N_Param);
        if (P_cur(p)->kind == TK_IDENT) { prm->s = P_cur(p)->text; p->p++; }
        else prm->s = intern("_");
        while (P_accept(p, "[")) { while (!P_is(p, "]") && P_cur(p)->kind != TK_EOF) p->p++; P_expect(p, "]", "in the parameter"); bputs(&ty, "[]"); }
        prm->type = intern(ty.s);
        prm->flags = flags;
        free(ty.s);
        node_add(ps, prm);
        if (!P_accept(p, ",")) break;
    }
    P_expect(p, ")", "to close the parameter list");
    return ps;
}

static Node *struct_body(Parser *p, Token *t, const char *name) {
    Node *s = mk(N_StructDef, t, name);
    P_expect(p, "{", "to start the struct body");
    while (!P_is(p, "}") && P_cur(p)->kind != TK_EOF) {
        int before = p->p;
        if (c_type_start(p, 0)) node_add(s, local_decl(p, 1));
        else { P_error(p, P_cur(p), "Syntax Error", NULL, "expected a member declaration"); }
        if (p->panic) sync_statement(p);
        if (p->p == before) p->p++;
    }
    P_expect(p, "}", "to close the struct body");
    return s;
}

static Node *enum_body(Parser *p, Token *t, const char *name) {
    Node *e = mk(N_EnumDef, t, name);
    P_expect(p, "{", "to start the enum body");
    while (P_cur(p)->kind == TK_IDENT) {
        Node *en = mk(N_Enumerator, P_cur(p), P_cur(p)->text);
        p->p++;
        if (P_accept(p, "=")) node_add(en, ternary(p));
        node_add(e, en);
        if (!P_accept(p, ",")) break;
    }
    P_expect(p, "}", "to close the enum");
    return e;
}

static Node *c_external(Parser *p) {
    Token *t = P_cur(p);
    if (t->kind == TK_DIRECTIVE) {
        p->p++;
        Node *d = mk(N_Directive, t, t->text);
        P_trace(p, "line %d: preprocessor directive %.30s", t->line, t->text);
        return d;
    }
    if (P_is(p, ";")) { p->p++; return NULL; }
    int is_typedef = P_accept(p, "typedef");
    int flags = 0;
    Token *spec_tok = P_cur(p);
    const char *base = c_specifiers(p, &flags);
    Node *def = NULL;
    if (P_is(p, "{")) {
        /* struct / union / enum definition */
        const char *tag = strchr(base, ' ') ? strchr(base, ' ') + 1 : NULL;
        if (strncmp(base, "enum", 4) == 0) def = enum_body(p, spec_tok, tag ? intern(tag) : NULL);
        else def = struct_body(p, spec_tok, tag ? intern(tag) : NULL);
        P_trace(p, "line %d: %s definition", spec_tok->line, base);
    }
    if (P_accept(p, ";")) {
        if (is_typedef) P_error(p, P_cur(p), "Syntax Error", NULL, "typedef without a name");
        return def ? def : unsupported(p, t, "empty declaration");
    }
    if (is_typedef) {
        /* typedef <base> [*]Name; -- remember Name as a type */
        Node *g = P_node(p, N_Block);
        do {
            while (P_accept(p, "*")) {}
            if (P_is(p, "(")) { P_error(p, P_cur(p), "Unsupported Construct", NULL, "function-pointer typedefs are not supported"); return g; }
            if (P_cur(p)->kind == TK_IDENT) { vpush(p->typedefs, P_cur(p)->text); p->p++; }
            while (P_is(p, "[")) skip_balanced(p, "[", "]");
        } while (P_accept(p, ","));
        P_expect(p, ";", "after the typedef");
        if (def) node_add(g, def);
        return g;
    }
    /* function definition / prototype, or global variables */
    int save = p->p;
    int f2 = flags;
    Buf ty = {0};
    bputs(&ty, base);
    while (P_is(p, "*")) { bputc(&ty, '*'); f2 |= NF_POINTER; p->p++; }
    Token *name = P_cur(p);
    if (name->kind == TK_IDENT && P_is_at(p, 1, "(")) {
        p->p++;
        Node *fn = mk(N_Function, name, name->text);
        fn->type = intern(ty.s);
        fn->flags = f2;
        free(ty.s);
        node_add(fn, c_params(p));
        if (P_accept(p, ";")) { fn->flags |= NF_PROTOTYPE; P_trace(p, "line %d: prototype of %s()", name->line, name->text); return fn; }
        P_trace(p, "line %d: function %s() with %d parameter%s", name->line, name->text, fn->k.v[0]->k.n, fn->k.v[0]->k.n == 1 ? "" : "s");
        node_add(fn, block(p));
        return fn;
    }
    free(ty.s);
    p->p = save;
    Node *g = decl_group(p, base, flags | NF_STATIC, 1);
    if (def) { Node *b = node_new(N_Block, def->line, def->col); node_add(b, def); node_add(b, g); return b; }
    return g;
}

/* ---- Java top level ---- */

static Node *java_params(Parser *p) {
    Node *ps = P_node(p, N_Params);
    P_expect(p, "(", "to start the parameter list");
    while (!P_is(p, ")") && P_cur(p)->kind != TK_EOF && !p->panic) {
        while (P_is(p, "final") || P_is(p, "@")) { if (P_is(p, "@")) skip_annotation(p); else p->p++; }
        Node *prm = P_node(p, N_Param);
        prm->type = java_type(p);
        if (!prm->type) { P_error(p, P_cur(p), "Syntax Error", NULL, "expected a parameter type, found '%s'", P_cur(p)->text); break; }
        if (P_cur(p)->kind == TK_IDENT) { prm->s = P_cur(p)->text; p->p++; }
        else P_error(p, P_cur(p), "Syntax Error", NULL, "expected a parameter name");
        while (P_is(p, "[") && P_is_at(p, 1, "]")) { p->p += 2; prm->type = intern(prm->type); }
        node_add(ps, prm);
        if (!P_accept(p, ",")) break;
    }
    P_expect(p, ")", "to close the parameter list");
    return ps;
}

static Node *java_class(Parser *p, Token *t);

static Node *java_member(Parser *p, const char *cls) {
    Token *t = P_cur(p);
    int flags = java_modifiers(p);
    if (P_is(p, ";")) { p->p++; return NULL; }
    if (P_is(p, "class") || P_is(p, "interface") || P_is(p, "enum") || P_is(p, "record") || (P_is(p, "@") && P_is_at(p, 1, "interface"))) {
        Node *c = java_class(p, t);
        if (c) c->flags |= flags;
        return c;
    }
    if (P_is(p, "{")) {                                     /* initializer block */
        Node *b = block(p);
        Node *fn = mk(N_Function, t, intern((flags & NF_STATIC) ? "<clinit-block>" : "<init-block>"));
        fn->flags = flags;
        fn->type = intern("void");
        node_add(fn, node_new(N_Params, t->line, t->col));
        node_add(fn, b);
        return fn;
    }
    if (P_is(p, "<")) skip_balanced(p, "<", ">");            /* generic method type parameters */
    /* constructor: Name ( */
    if (P_cur(p)->kind == TK_IDENT && strcmp(P_cur(p)->text, cls) == 0 && P_is_at(p, 1, "(")) {
        Token *nt = P_cur(p);
        p->p++;
        Node *fn = mk(N_Function, nt, intern("<init>"));
        fn->type = intern("void");
        fn->flags = flags | NF_METHOD;
        node_add(fn, java_params(p));
        if (P_accept(p, "throws")) { while (!P_is(p, "{") && P_cur(p)->kind != TK_EOF) p->p++; }
        node_add(fn, block(p));
        P_trace(p, "line %d: constructor of %s", nt->line, cls);
        return fn;
    }
    const char *ty = java_type(p);
    if (!ty) {
        P_error(p, P_cur(p), "Syntax Error", "Members are fields, methods, constructors or nested classes.",
                "unexpected %s in the body of class %s", P_cur(p)->text, cls);
        return NULL;
    }
    Token *nt = P_cur(p);
    if (nt->kind == TK_IDENT && P_is_at(p, 1, "(")) {
        p->p++;
        Node *fn = mk(N_Function, nt, nt->text);
        fn->type = ty;
        fn->flags = flags | ((flags & NF_STATIC) ? 0 : NF_METHOD);
        node_add(fn, java_params(p));
        while (P_is(p, "[") && P_is_at(p, 1, "]")) p->p += 2;
        if (P_accept(p, "throws")) { while (!P_is(p, "{") && !P_is(p, ";") && P_cur(p)->kind != TK_EOF) p->p++; }
        if (P_accept(p, ";")) { fn->flags |= NF_PROTOTYPE; return fn; }
        P_trace(p, "line %d: method %s.%s() with %d parameter%s", nt->line, cls, nt->text, fn->k.v[0]->k.n, fn->k.v[0]->k.n == 1 ? "" : "s");
        node_add(fn, block(p));
        return fn;
    }
    Node *g = decl_group(p, ty, flags, 1);
    return g;
}

static Node *java_class(Parser *p, Token *t) {
    Token *kw = P_cur(p);
    if (P_is(p, "@")) p->p++;
    p->p++;                                                   /* class / interface / enum / record */
    Token *name = P_cur(p);
    if (name->kind != TK_IDENT) { P_error(p, name, "Syntax Error", NULL, "expected a class name"); return NULL; }
    p->p++;
    if (strcmp(kw->text, "class")) {
        /* interfaces, enums and records: parsed for structure, not translated */
        while (!P_is(p, "{") && P_cur(p)->kind != TK_EOF) p->p++;
        skip_balanced(p, "{", "}");
        char what[64];
        snprintf(what, sizeof(what), "%s %s", kw->text, name->text);
        P_trace(p, "line %d: %s (body skipped)", kw->line, what);
        return unsupported(p, t, intern(what));
    }
    Node *c = mk(N_Class, name, name->text);
    if (P_is(p, "<")) skip_balanced(p, "<", ">");
    if (P_accept(p, "extends")) { java_type(p); }
    if (P_accept(p, "implements")) { do java_type(p); while (P_accept(p, ",")); }
    if (P_accept(p, "permits")) { do java_type(p); while (P_accept(p, ",")); }
    P_trace(p, "line %d: class %s", name->line, name->text);
    if (!P_expect(p, "{", "to start the class body")) return c;
    while (!P_is(p, "}") && P_cur(p)->kind != TK_EOF) {
        int before = p->p;
        Node *m = java_member(p, name->text);
        if (m) node_add(c, m);
        if (p->panic) sync_statement(p);
        if (p->p == before) p->p++;
    }
    P_expect(p, "}", "to close the class body");
    return c;
}

static Node *java_external(Parser *p) {
    Token *t = P_cur(p);
    if (P_is(p, "package") || P_is(p, "import")) {
        int start = t->pos;
        while (!P_is(p, ";") && P_cur(p)->kind != TK_EOF) p->p++;
        Token *last = P_cur(p);
        P_expect(p, ";", "after the declaration");
        Node *n = mk(t->text[0] == 'p' ? N_Directive : N_Import, t, intern_n(p->src + start, (size_t)(last->pos - start)));
        return n;
    }
    if (P_is(p, ";")) { p->p++; return NULL; }
    int flags = java_modifiers(p);
    if (P_is(p, "class") || P_is(p, "interface") || P_is(p, "enum") || P_is(p, "record") || P_is(p, "@")) {
        Node *c = java_class(p, t);
        if (c) c->flags |= flags;
        return c;
    }
    P_error(p, t, "Syntax Error", "Java code must be inside a class.", "expected a class or interface declaration, found '%s'", t->text);
    return NULL;
}

Node *parse_c_family(Parser *p) {
    Node *prog = node_new(N_Program, 1, 1);
    while (P_cur(p)->kind != TK_EOF) {
        int before = p->p;
        Node *n = p->lang == LANG_C ? c_external(p) : java_external(p);
        if (n) node_add(prog, n);
        if (p->panic) sync_statement(p);
        if (p->p == before) p->p++;
    }
    return prog;
}
