#include "automata.h"

/* ---- alphabet equivalence classes ----
 * Start with every byte in one class, then split classes by each byte set
 * a CHAR or CLASS state tests. Afterwards every state's set is a union of
 * whole classes, so one representative byte per class decides a move. */
void alphabet_classes(const NFA *n, unsigned char sym_of[256], int sym_rep[256], int *nsyms) {
    int cls[256] = {0}, ncls = 1;
    int remap[512];
    for (int i = 0; i < n->s.n; i++) {
        const NState *s = &n->s.v[i];
        if (s->type != NS_CHAR && s->type != NS_CLASS) continue;
        for (int k = 0; k < ncls * 2; k++) remap[k] = -1;
        int next = 0;
        for (int c = 0; c < 256; c++) {
            int in = s->type == NS_CHAR ? (c == s->c) : cset_test(&n->cls.v[s->c], c);
            int key = cls[c] * 2 + in;
            if (remap[key] < 0) remap[key] = next++;
            cls[c] = remap[key];
        }
        ncls = next;
        if (ncls > 255) break;           /* every byte separate already */
    }
    for (int k = 0; k < ncls; k++) sym_rep[k] = -1;
    for (int c = 0; c < 256; c++) {
        sym_of[c] = (unsigned char)cls[c];
        if (sym_rep[cls[c]] < 0) sym_rep[cls[c]] = c;
    }
    *nsyms = ncls;
}

static int state_moves_on(const NFA *n, const NState *s, int byte) {
    if (s->type == NS_CHAR) return s->c == byte;
    if (s->type == NS_CLASS) return cset_test(&n->cls.v[s->c], byte);
    return 0;
}

/* ---- epsilon closure ---- */

static void closure_add(const NFA *n, int s, unsigned *mark, unsigned gen, IntVec *out, IntVec *stack) {
    stack->n = 0;
    vpush(*stack, s);
    while (stack->n) {
        int x = stack->v[--stack->n];
        if (x < 0 || mark[x] == gen) continue;
        mark[x] = gen;
        vpush(*out, x);
        const NState *st = &n->s.v[x];
        if (st->type == NS_SPLIT) { vpush(*stack, st->out1); vpush(*stack, st->out); }
    }
}

static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }

void nfa_closure(const NFA *n, int s, IntVec *out) {
    unsigned *mark = xcalloc((size_t)n->s.n, sizeof(unsigned));
    IntVec stack = {0};
    out->n = 0;
    closure_add(n, s, mark, 1, out, &stack);
    qsort(out->v, (size_t)out->n, sizeof(int), cmp_int);
    free(mark);
    vfree(stack);
}

/* ---- subset construction ---- */

/* The DFA under construction: its NFA-state sets, a hash index over them,
 * and the transition / accept tables that grow with it. */
typedef struct {
    VEC(IntVec) sets;
    IntVec trans, accept;
    int *keys, cap;
    int nsyms;
    const NFA *nfa;
    const int *prio;
} Builder;

static size_t hash_ints(const int *v, int n) {
    size_t h = 1469598103934665603ULL ^ (size_t)n;
    for (int i = 0; i < n; i++) { h ^= (size_t)v[i] + 0x9e3779b9; h *= 1099511628211ULL; }
    return h;
}

static int best_rule(const NFA *n, const IntVec *set, const int *prio) {
    int best = -1;
    for (int i = 0; i < set->n; i++) {
        const NState *s = &n->s.v[set->v[i]];
        if (s->type != NS_MATCH) continue;
        int r = s->rule;
        if (best < 0) { best = r; continue; }
        int pr = prio ? prio[r] : 0, pb = prio ? prio[best] : 0;
        if (pr < pb || (pr == pb && r < best)) best = r;
    }
    return best;
}

static void builder_rehash(Builder *b, int ncap) {
    int *keys = xmalloc(sizeof(int) * (size_t)ncap);
    for (int i = 0; i < ncap; i++) keys[i] = -1;
    for (int q = 0; q < b->sets.n; q++) {
        size_t h = hash_ints(b->sets.v[q].v, b->sets.v[q].n) & (size_t)(ncap - 1);
        while (keys[h] >= 0) h = (h + 1) & (size_t)(ncap - 1);
        keys[h] = q;
    }
    free(b->keys);
    b->keys = keys;
    b->cap = ncap;
}

/* Finds the DFA state for a sorted NFA-state set, adding it if new. */
static int builder_lookup(Builder *b, const IntVec *set) {
    size_t h = hash_ints(set->v, set->n) & (size_t)(b->cap - 1);
    while (b->keys[h] >= 0) {
        const IntVec *o = &b->sets.v[b->keys[h]];
        if (o->n == set->n && memcmp(o->v, set->v, sizeof(int) * (size_t)set->n) == 0) return b->keys[h];
        h = (h + 1) & (size_t)(b->cap - 1);
    }
    IntVec copy = {0};
    for (int q = 0; q < set->n; q++) vpush(copy, set->v[q]);
    vpush(b->sets, copy);
    int id = b->sets.n - 1;
    b->keys[h] = id;
    vpush(b->accept, best_rule(b->nfa, &copy, b->prio));
    for (int a = 0; a < b->nsyms; a++) vpush(b->trans, -1);
    if (b->sets.n * 2 > b->cap) builder_rehash(b, b->cap * 2);
    return id;
}

int dfa_build(DFA *d, const NFA *n, int start, const int *prio, int max_states) {
    memset(d, 0, sizeof(*d));
    alphabet_classes(n, d->sym_of, d->sym_rep, &d->nsyms);
    int ns = d->nsyms;

    Builder b = {0};
    b.nsyms = ns; b.nfa = n; b.prio = prio;
    builder_rehash(&b, 64);
    IntVec stack = {0};
    unsigned *mark = xcalloc((size_t)n->s.n, sizeof(unsigned));
    unsigned gen = 0;

    IntVec *bucket = xcalloc((size_t)ns, sizeof(IntVec));
    int failed = 0;


    IntVec cur = {0};
    gen++;
    closure_add(n, start, mark, gen, &cur, &stack);
    qsort(cur.v, (size_t)cur.n, sizeof(int), cmp_int);
    d->start = builder_lookup(&b, &cur);

    for (int di = 0; di < b.sets.n; di++) {
        for (int a = 0; a < ns; a++) bucket[a].n = 0;
        /* move: every consuming state adds its target to each symbol it accepts */
        for (int k = 0; k < b.sets.v[di].n; k++) {
            const NState *st = &n->s.v[b.sets.v[di].v[k]];
            if (st->type == NS_CHAR) { vpush(bucket[d->sym_of[st->c]], st->out); }
            else if (st->type == NS_CLASS)
                for (int a = 0; a < ns; a++)
                    if (state_moves_on(n, st, d->sym_rep[a])) vpush(bucket[a], st->out);
        }
        for (int a = 0; a < ns; a++) {
            if (!bucket[a].n || d->sym_rep[a] == 0) continue;     /* byte 0 is end of input */
            cur.n = 0;
            gen++;
            for (int k = 0; k < bucket[a].n; k++) closure_add(n, bucket[a].v[k], mark, gen, &cur, &stack);
            qsort(cur.v, (size_t)cur.n, sizeof(int), cmp_int);
            int t = builder_lookup(&b, &cur);
            b.trans.v[di * ns + a] = t;
            if (b.sets.n > max_states) { failed = 1; break; }
        }
        if (failed) break;
    }

    d->nstates = b.sets.n;
    d->trans = b.trans.v;
    d->accept = b.accept.v;
    d->members = b.sets.v;
    for (int a = 0; a < ns; a++) vfree(bucket[a]);
    free(bucket); free(mark); free(b.keys);
    vfree(cur); vfree(stack);
    return failed ? -1 : 0;
}

/* ---- minimization ---- */

void dfa_minimize(DFA *out, const DFA *in) {
    int n = in->nstates, ns = in->nsyms;
    memset(out, 0, sizeof(*out));
    memcpy(out->sym_of, in->sym_of, sizeof(in->sym_of));
    memcpy(out->sym_rep, in->sym_rep, sizeof(in->sym_rep));
    out->nsyms = ns;

    /* 1. unreachable states (subset construction never makes any, but a
     *    minimizer should not rely on how its input was built) */
    char *reach = xcalloc((size_t)n, 1);
    IntVec stack = {0};
    vpush(stack, in->start);
    reach[in->start] = 1;
    while (stack.n) {
        int s = stack.v[--stack.n];
        for (int a = 0; a < ns; a++) {
            int t = in->trans[s * ns + a];
            if (t >= 0 && !reach[t]) { reach[t] = 1; vpush(stack, t); }
        }
    }
    int removed = 0;
    for (int i = 0; i < n; i++) removed += !reach[i];

    /* 2. initial partition: one block per accepted rule (+ non-accepting) */
    int *block = xmalloc(sizeof(int) * (size_t)n), *nb = xmalloc(sizeof(int) * (size_t)n);
    int nblocks = 0;
    {
        int maxr = 0;
        for (int i = 0; i < n; i++) if (in->accept[i] + 1 > maxr) maxr = in->accept[i] + 1;
        int *id = xmalloc(sizeof(int) * (size_t)(maxr + 1));
        for (int r = 0; r <= maxr; r++) id[r] = -1;
        for (int i = 0; i < n; i++) {
            if (!reach[i]) { block[i] = -1; continue; }
            int key = in->accept[i] + 1;
            if (id[key] < 0) id[key] = nblocks++;
            block[i] = id[key];
        }
        free(id);
    }

    /* 3. refine: a state's signature is its block plus the block each symbol leads to */
    int rounds = 0;
    int *sig = xmalloc(sizeof(int) * (size_t)(ns + 1));
    int cap = 64;
    while (cap < n * 2) cap *= 2;
    int *tab = xmalloc(sizeof(int) * (size_t)cap);
    for (;;) {
        rounds++;
        for (int i = 0; i < cap; i++) tab[i] = -1;
        int count = 0;
        for (int i = 0; i < n; i++) {
            if (block[i] < 0) { nb[i] = -1; continue; }
            sig[0] = block[i];
            for (int a = 0; a < ns; a++) { int t = in->trans[i * ns + a]; sig[a + 1] = t < 0 ? -1 : block[t]; }
            size_t h = hash_ints(sig, ns + 1) & (size_t)(cap - 1);
            for (;;) {
                int j = tab[h];
                if (j < 0) { tab[h] = i; nb[i] = count++; break; }
                /* compare with representative j */
                int same = block[j] == block[i];
                for (int a = 0; same && a < ns; a++) {
                    int t1 = in->trans[i * ns + a], t2 = in->trans[j * ns + a];
                    same = (t1 < 0 ? -1 : block[t1]) == (t2 < 0 ? -1 : block[t2]);
                }
                if (same) { nb[i] = nb[j]; break; }
                h = (h + 1) & (size_t)(cap - 1);
            }
        }
        memcpy(block, nb, sizeof(int) * (size_t)n);
        if (count == nblocks) break;
        nblocks = count;
    }

    /* 4. build the quotient automaton, numbering blocks in BFS order from the start */
    int *order = xmalloc(sizeof(int) * (size_t)(nblocks ? nblocks : 1));
    int *rep = xmalloc(sizeof(int) * (size_t)(nblocks ? nblocks : 1));
    for (int b = 0; b < nblocks; b++) { order[b] = -1; rep[b] = -1; }
    for (int i = 0; i < n; i++) if (block[i] >= 0 && rep[block[i]] < 0) rep[block[i]] = i;
    int next = 0;
    IntVec q = {0};
    order[block[in->start]] = next++;
    vpush(q, block[in->start]);
    for (int qi = 0; qi < q.n; qi++) {
        int b = q.v[qi];
        for (int a = 0; a < ns; a++) {
            int t = in->trans[rep[b] * ns + a];
            if (t >= 0 && order[block[t]] < 0) { order[block[t]] = next++; vpush(q, block[t]); }
        }
    }
    out->nstates = nblocks;
    out->start = 0;
    out->trans = xmalloc(sizeof(int) * (size_t)(nblocks * ns ? nblocks * ns : 1));
    out->accept = xmalloc(sizeof(int) * (size_t)(nblocks ? nblocks : 1));
    out->members = xcalloc((size_t)(nblocks ? nblocks : 1), sizeof(IntVec));
    for (int b = 0; b < nblocks; b++) {
        int o = order[b], r = rep[b];
        out->accept[o] = in->accept[r];
        for (int a = 0; a < ns; a++) {
            int t = in->trans[r * ns + a];
            out->trans[o * ns + a] = t < 0 ? -1 : order[block[t]];
        }
    }
    for (int i = 0; i < n; i++) if (block[i] >= 0) vpush(out->members[order[block[i]]], i);
    out->removed_unreachable = removed;
    out->refine_rounds = rounds;

    free(reach); free(block); free(nb); free(sig); free(tab); free(order); free(rep);
    vfree(stack); vfree(q);
}

int dfa_transitions(const DFA *d) {
    int c = 0;
    for (int i = 0; i < d->nstates * d->nsyms; i++) c += d->trans[i] >= 0;
    return c;
}

void dfa_free(DFA *d) {
    for (int i = 0; i < d->nstates; i++) vfree(d->members[i]);
    free(d->members); free(d->trans); free(d->accept);
    memset(d, 0, sizeof(*d));
}

/* ---- recognisers ---- */

static void sim_add(NfaSim *sim, const NFA *n, IntVec *set, int s) {
    /* iterative: deep epsilon chains (long keyword lists) would overflow recursion */
    int stackbuf[64];
    int *stack = stackbuf, cap = 64, top = 0;
    stack[top++] = s;
    while (top) {
        int x = stack[--top];
        if (x < 0 || sim->mark[x] == sim->gen) continue;
        sim->mark[x] = sim->gen;
        const NState *ns = &n->s.v[x];
        if (ns->type == NS_SPLIT) {
            if (top + 2 > cap) {
                int *bigger = xmalloc(sizeof(int) * (size_t)cap * 2);
                memcpy(bigger, stack, sizeof(int) * (size_t)top);
                if (stack != stackbuf) free(stack);
                stack = bigger; cap *= 2;
            }
            stack[top++] = ns->out1;
            stack[top++] = ns->out;
        } else {
            vpush(*set, x);
        }
    }
    if (stack != stackbuf) free(stack);
}

static void sim_next_gen(NfaSim *sim, int nstates) {
    if (sim->cap < nstates) {
        free(sim->mark);
        sim->mark = xcalloc((size_t)nstates, sizeof(unsigned));
        sim->cap = nstates;
        sim->gen = 0;
    }
    if (++sim->gen == 0) { memset(sim->mark, 0, sizeof(unsigned) * (size_t)sim->cap); sim->gen = 1; }
}

static int set_rule(const NFA *n, const IntVec *set, const int *prio) {
    int best = -1;
    for (int j = 0; j < set->n; j++) {
        const NState *s = &n->s.v[set->v[j]];
        if (s->type != NS_MATCH) continue;
        int pr = prio ? prio[s->rule] : 0, pb = best >= 0 && prio ? prio[best] : 0;
        if (best < 0 || pr < pb || (pr == pb && s->rule < best)) best = s->rule;
    }
    return best;
}

int nfa_longest(NfaSim *sim, const NFA *n, int start, const char *text, const int *prio, int *rule) {
    IntVec *cur = &sim->a, *next = &sim->b;
    cur->n = 0;
    sim_next_gen(sim, n->s.n);
    sim_add(sim, n, cur, start);
    int best = 0, best_rule = -1;
    for (int i = 0; text[i] && cur->n; i++) {
        unsigned char c = (unsigned char)text[i];
        next->n = 0;
        sim_next_gen(sim, n->s.n);
        for (int j = 0; j < cur->n; j++) {
            const NState *s = &n->s.v[cur->v[j]];
            if (state_moves_on(n, s, c)) sim_add(sim, n, next, s->out);
        }
        IntVec *t = cur; cur = next; next = t;
        int r = set_rule(n, cur, prio);
        if (r >= 0) { best = i + 1; best_rule = r; }
    }
    if (rule) *rule = best_rule;
    return best;
}

int dfa_longest(const DFA *d, const char *text, int *rule) {
    int s = d->start, best = 0, best_rule = -1;
    for (int i = 0; text[i]; i++) {
        s = d->trans[s * d->nsyms + d->sym_of[(unsigned char)text[i]]];
        if (s < 0) break;
        if (d->accept[s] >= 0) { best = i + 1; best_rule = d->accept[s]; }
    }
    if (rule) *rule = best_rule;
    return best;
}

void nfasim_free(NfaSim *sim) { free(sim->mark); vfree(sim->a); vfree(sim->b); memset(sim, 0, sizeof(*sim)); }
