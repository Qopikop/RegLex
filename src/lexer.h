/* Rule sets and the scanner built from them.
 *
 * A rules file has one rule per line:   NAME  PRIORITY  token|skip  REGEX
 * Every rule is compiled to its own Thompson NFA (for display) and into one
 * combined NFA; the combined NFA is converted to a DFA and minimized. The
 * scanner can run on any of the three -- they must agree, and the tests and
 * the web page check that they do.
 */
#ifndef LEXER_H
#define LEXER_H

#include "automata.h"

typedef struct {
    char *name, *pattern;
    int prio, skip, line;
    NFA nfa;          /* this rule alone */
    int start;
    Postfix pf;
    int never_wins;   /* set after the DFA is built: no state accepts this rule */
    int matches_empty;
} Rule;

typedef struct {
    VEC(Rule) r;
    const char *name;          /* e.g. "rules/c.rules" */
    NFA comb;                  /* all rules together */
    int comb_start;
    int *prio;
    DFA dfa, min;
    int dfa_ok;
    double ms_nfa, ms_dfa, ms_min;   /* construction times */
} RuleSet;

typedef enum {
    TK_KEYWORD, TK_IDENT, TK_NUMBER, TK_STRING, TK_CHAR, TK_OP, TK_PUNCT,
    TK_DIRECTIVE, TK_OTHER, TK_NEWLINE, TK_INDENT, TK_DEDENT, TK_EOF, TK_SKIP
} TokKind;

typedef struct {
    TokKind kind;
    int rule;                 /* rule index, -1 for synthetic tokens */
    int pos, len, line, col;
    const char *text;         /* interned lexeme */
} Token;
typedef VEC(Token) Tokens;

typedef enum { SCAN_NFA, SCAN_DFA, SCAN_MIN } ScanMode;

#define MAX_DFA_STATES 50000

int  rules_parse(RuleSet *rs, const char *text, const char *name, Diags *ds);   /* -1 on error */
int  rules_build(RuleSet *rs, Diags *ds);
void rules_free(RuleSet *rs);
const char *tok_kind_name(TokKind k);

/* Scans src. Skipped matches are kept (kind TK_SKIP) only if keep_skipped.
 * Returns the number of lexical errors. */
int  lex(RuleSet *rs, const char *src, ScanMode mode, Tokens *out, Diags *ds, int keep_skipped);

/* Python: turns line structure into NEWLINE / INDENT / DEDENT tokens.
 * This is a counter-based pass over the DFA's tokens -- indentation is not a
 * regular language, so no finite automaton can do it alone. */
void python_layout(const Tokens *in, const char *src, Tokens *out, Diags *ds);

/* C / Java: drops skipped tokens and adds EOF. */
void plain_layout(const Tokens *in, Tokens *out);

#endif
