/* Recursive-descent parsers for C, Java and Python. They read the tokens
 * produced by the DFA scanner (plus Python's layout tokens) and build the
 * shared AST. On a syntax error they report it, skip to a safe point
 * (end of statement or block) and keep going, so one mistake does not hide
 * the rest of the file. */
#ifndef PARSE_H
#define PARSE_H

#include "lexer.h"
#include "ast.h"

typedef enum { LANG_C, LANG_JAVA, LANG_PY } Lang;

typedef struct {
    Tokens *t;
    int p;
    Diags *ds;
    Lang lang;
    int panic;                    /* inside error recovery: suppress follow-on errors */
    int depth;
    RuleSet *rs;                  /* to scan f-string expressions (Python) */
    const char *src;
    VEC(const char *) typedefs;   /* C typedef names */
    VEC(const char *) trace;      /* parser decisions, for the execution trace */
    int trace_limit;
} Parser;

Node *parse_program(Parser *p);   /* dispatches on p->lang */

/* shared helpers (parse.c) */
Token *P_cur(Parser *p);
Token *P_peek(Parser *p, int ahead);
int    P_is(Parser *p, const char *text);
int    P_is_at(Parser *p, int ahead, const char *text);
int    P_accept(Parser *p, const char *text);
int    P_expect(Parser *p, const char *text, const char *where);
void   P_error(Parser *p, Token *at, const char *category, const char *hint, const char *fmt, ...);
void   P_trace(Parser *p, const char *fmt, ...);
int    P_enter(Parser *p);        /* recursion guard; 0 when too deep */
void   P_leave(Parser *p);
Node  *P_node(Parser *p, NodeKind k);   /* new node at the current token */

/* literal decoding */
long long  parse_int_literal(const char *text, int *ok);
double     parse_float_literal(const char *text);
int        is_float_literal(const char *text, Lang lang);
const char *decode_string(const char *text, Lang lang, int *is_fstring, int *is_bytes);
long long  decode_char(const char *text);

Node *parse_c_family(Parser *p);
Node *parse_python(Parser *p);

#endif
