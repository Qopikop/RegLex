/* AST -> three-address code, with semantic analysis on the way:
 * a scoped symbol table, undeclared / duplicate names, type and arity
 * checks, and language-specific conversions (C usual arithmetic
 * conversions, Java string concatenation, Python dynamic values). */
#ifndef LOWER_H
#define LOWER_H

#include "ir.h"

typedef struct {
    const char *name, *kind, *type, *scope;
    int line, col, uses;
} SymInfo;

typedef struct {
    VEC(SymInfo) syms;
    VEC(char *) trace;          /* "line 12: while loop -> 9 instructions" */
    int opaque;                 /* constructs kept as OPAQUE instructions */
    int complete;               /* 1 if nothing had to be left opaque */
} LowerInfo;

int lower_program(Node *ast, Lang lang, IRProg *out, Diags *ds, LowerInfo *info);

#endif
