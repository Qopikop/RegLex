/* Regular expressions -> postfix -> Thompson NFA.
 *
 * Supported syntax (the whole of it -- this is a teaching subset, not PCRE):
 *   ab        concatenation           a|b     alternation (empty sides allowed)
 *   a*  a+  a?  repetition            (a)     grouping, () is the empty string
 *   [abc] [a-z] [^...]  classes       .       any byte except newline
 *   \x        the literal x (also \n \t \r \f \v for those characters)
 * Not supported: {m,n} counts, anchors, backreferences, lookaround, \d \w.
 */
#ifndef REGEX_H
#define REGEX_H

#include "util.h"

typedef enum { NS_CHAR, NS_CLASS, NS_SPLIT, NS_MATCH } NStateType;

typedef struct { unsigned char b[32]; } CSet;   /* 256-bit byte set */

/* One NFA state. CHAR/CLASS consume a byte and go to `out`. SPLIT is an
 * epsilon state going to `out` and (if >= 0) `out1`. MATCH accepts `rule`. */
typedef struct {
    unsigned char type;
    int c;              /* byte (CHAR) or class index (CLASS) */
    int out, out1;
    int rule;           /* MATCH only */
} NState;

typedef struct {
    VEC(NState) s;
    VEC(CSet) cls;
} NFA;

typedef enum { PF_LIT, PF_CLASS, PF_EPS, PF_CAT, PF_ALT, PF_STAR, PF_PLUS, PF_OPT } PfKind;
typedef struct { unsigned char k; int v; } PfItem;
typedef VEC(PfItem) Postfix;

static inline int cset_test(const CSet *s, int c) { return (s->b[c >> 3] >> (c & 7)) & 1; }
static inline void cset_add(CSet *s, int c) { s->b[c >> 3] |= (unsigned char)(1u << (c & 7)); }

/* Parses `re`, appends its classes to nfa->cls and its postfix to *pf.
 * Returns 0, or -1 with a message (including the position) in err. */
int  regex_to_postfix(const char *re, NFA *nfa, Postfix *pf, char *err, size_t errlen);

/* Thompson's construction. Returns the start state; adds one MATCH(rule). */
int  nfa_from_postfix(NFA *nfa, const Postfix *pf, int rule);

/* Both steps. Returns the start state or -1 (err filled). pf may be NULL. */
int  regex_compile(NFA *nfa, const char *re, int rule, Postfix *pf, char *err, size_t errlen);

/* Readable form of a class, e.g. "[a-zA-Z_]" or "[^\n]". */
void cset_label(const CSet *s, char *buf, size_t n);
const char *show_byte(int c, char *buf);   /* "a", "\\n", "\\x01" */

void nfa_free(NFA *nfa);

#endif
