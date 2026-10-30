/* NFA simulation, NFA -> DFA (subset construction) and DFA minimization.
 *
 * Several rules share one NFA: each rule's fragment ends in its own MATCH
 * state. A DFA state accepts the rule with the best (lowest) priority among
 * the MATCH states in its NFA set, ties going to the earlier rule -- the same
 * tie-break the scanner uses, so all three automata produce the same tokens.
 */
#ifndef AUTOMATA_H
#define AUTOMATA_H

#include "regex.h"

typedef struct {
    int nstates, nsyms, start;
    unsigned char sym_of[256];   /* byte -> input symbol (equivalence class of bytes) */
    int sym_rep[256];            /* symbol -> one byte in it */
    int *trans;                  /* nstates * nsyms, -1 = no move (dead) */
    int *accept;                 /* rule index, or -1 */
    IntVec *members;             /* DFA: the NFA state set; minimized: the DFA states merged */
    int removed_unreachable;     /* minimization only */
    int refine_rounds;           /* minimization only */
} DFA;

/* Alphabet: groups bytes no state can tell apart into one symbol. */
void alphabet_classes(const NFA *n, unsigned char sym_of[256], int sym_rep[256], int *nsyms);

/* Subset construction from `start`. prio[r] is rule r's priority (NULL: all 0).
 * Returns 0, or -1 if more than max_states would be needed. */
int  dfa_build(DFA *d, const NFA *n, int start, const int *prio, int max_states);

/* Moore's partition refinement: start from {accepting rule} groups, split
 * groups whose members move to different groups, until nothing changes. */
void dfa_minimize(DFA *out, const DFA *in);

int  dfa_transitions(const DFA *d);
void dfa_free(DFA *d);

/* Longest-match recognisers. Return the length of the longest non-empty
 * accepted prefix (0 if none) and the rule that accepts it. */
typedef struct { unsigned *mark; unsigned gen; IntVec a, b; int cap; } NfaSim;
int  nfa_longest(NfaSim *sim, const NFA *n, int start, const char *text, const int *prio, int *rule);
int  dfa_longest(const DFA *d, const char *text, int *rule);
void nfasim_free(NfaSim *sim);

/* Epsilon closure of {s}, sorted (for display / teaching). */
void nfa_closure(const NFA *n, int s, IntVec *out);

#endif
