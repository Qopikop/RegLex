/* Executes three-address code. Used to check the optimizer: the original
 * and the optimized TAC are both run and their output compared, and the
 * test suite compares the original against the real gcc / javac / python3.
 *
 * It is a sandbox by construction: there are no system calls, only a small
 * set of library functions (printing, math, strings, lists), and hard limits
 * on steps, recursion depth, heap size and output size. */
#ifndef INTERP_H
#define INTERP_H

#include "ir.h"

typedef enum {
    RUN_OK,            /* finished normally */
    RUN_EXCEPTION,     /* the program raised an error the language defines (IndexError, ArithmeticException...) */
    RUN_UNSUPPORTED,   /* hit something the interpreter does not model (opaque op, unknown library call) */
    RUN_LIMIT          /* step / depth / memory / output limit */
} RunStatus;

typedef struct {
    RunStatus status;
    char msg[512];
    Buf out;
    long long steps;
    int exit_code;
    int ub_count;            /* C undefined behaviour seen (e.g. signed overflow) */
    char ub_first[256];
    double ms;
} RunResult;

typedef struct { long long max_steps; int max_depth; size_t max_heap; size_t max_out; } RunLimits;

void ir_run(IRProg *p, const RunLimits *lim, RunResult *r);
const char *run_status_name(RunStatus s);

#endif
