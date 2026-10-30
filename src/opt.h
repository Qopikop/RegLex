/* Optimizer over the three-address code.
 *
 * Every pass works on the control-flow graph of one function and logs
 * each change it makes (what it was, what it became, and why). The web
 * page's "applied optimizations" list is built only from these records.
 * A pass only fires when the change is safe under the source language's
 * rules -- e.g. x * 0 -> 0 needs x to be a proven integer (not a float,
 * which could be NaN, nor a Python object with an overloaded '*').
 */
#ifndef OPT_H
#define OPT_H

#include "ir.h"

enum {
    OPT_UNREACH  = 1 << 0,   /* unreachable-code elimination */
    OPT_CFG      = 1 << 1,   /* branch folding, jump threading, dead labels */
    OPT_CONSTPROP= 1 << 2,   /* global constant propagation (dataflow) */
    OPT_FOLD     = 1 << 3,   /* constant folding */
    OPT_ALGEBRA  = 1 << 4,   /* algebraic simplification */
    OPT_STRENGTH = 1 << 5,   /* strength reduction */
    OPT_COPYPROP = 1 << 6,   /* global copy propagation (dataflow) */
    OPT_LVN      = 1 << 7,   /* local value numbering (basic-block CSE) */
    OPT_GCSE     = 1 << 8,   /* global CSE (available expressions) */
    OPT_LICM     = 1 << 9,   /* loop-invariant code motion */
    OPT_DCE      = 1 << 10,  /* dead-code elimination (liveness) */
    OPT_ALL      = (1 << 11) - 1
};

typedef struct {
    const char *pass, *func;
    int line, block, id;
    char *before, *after, *why;
} OptRecord;

typedef struct {
    VEC(OptRecord) rec;
    int rounds;
    int counts[11];              /* changes per pass */
    double ms;
} OptReport;

const char *opt_pass_name(int bit);
const char *opt_pass_key(int bit);       /* "fold", "constprop", ... */
int  opt_parse_passes(const char *list); /* "fold,dce" or "all" or "none" */
void optimize(IRProg *p, int passes, OptReport *rep);

/* Control-flow graph (also exported to the web page). */
typedef struct { int start, end; IntVec succ, pred; } BB;
typedef struct { BB *b; int n; int *block_of; } CFG;
void cfg_build(const Func *f, CFG *g);
void cfg_free(CFG *g);
/* back edges (u -> h where h dominates u): pairs in out */
void cfg_back_edges(const CFG *g, IntVec *out);

#endif
