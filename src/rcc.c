/* rcc -- the compiler workbench driver.
 *
 *   rcc program.c                       readable report on the terminal
 *   rcc program.py --json               everything as one JSON document (the web page uses this)
 *   rcc program.java --passes fold,dce  choose optimization passes (all / none / a list)
 *   rcc code.txt --rules my.rules --lex-only     custom lexical rules: tokens and automata only
 *   rcc --simulate IDENT --input abc_1 --rules rules/c.rules [--automaton nfa|dfa|min]
 *
 * Pipeline: rules -> Thompson NFA -> subset-construction DFA -> minimized DFA
 *           -> scanner (minimized DFA) -> parser -> AST + semantic checks -> TAC
 *           -> optimizer -> TAC interpreter (original vs optimized).
 */
#include "lower.h"
#include "opt.h"
#include "interp.h"
#include "value.h"
#include <ctype.h>
#include <sys/resource.h>

#define SCHEMA "compiler-workbench/1"

typedef struct {
    const char *src_path, *rules_path, *rules_dir, *lang_arg, *passes_arg, *simulate, *sim_input, *sim_automaton;
    int json, lex_only, no_run, scan_mode, show_tokens, max_tokens;
    long long max_steps;
} Options;

static void usage(void) {
    fprintf(stderr,
        "usage: rcc [source-file] [options]\n"
        "  --lang c|java|python|auto   language (default: from the file extension)\n"
        "  --rules FILE                lexical rules (default: rules/<lang>.rules)\n"
        "  --lex-only                  stop after lexical analysis (needed for custom rules)\n"
        "  --mode nfa|dfa|min          automaton used to scan (default: min)\n"
        "  --passes LIST               optimization passes: all, none, or e.g. constprop,fold,dce\n"
        "  --no-run                    do not execute the TAC\n"
        "  --max-steps N               interpreter step limit (default 20000000)\n"
        "  --json                      print one JSON document\n"
        "  --tokens                    list every token in the text report\n"
        "  --simulate RULE --input S [--automaton nfa|dfa|min]   step one rule's automaton over S\n");
    exit(2);
}

static Options parse_args(int argc, char **argv) {
    Options o;
    memset(&o, 0, sizeof(o));
    o.rules_dir = "rules";
    o.scan_mode = SCAN_MIN;
    o.max_tokens = 20000;
    o.max_steps = 20000000;
    o.sim_automaton = "dfa";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (usage(), ""))
        if (!strcmp(a, "--json")) o.json = 1;
        else if (!strcmp(a, "--lex-only")) o.lex_only = 1;
        else if (!strcmp(a, "--no-run")) o.no_run = 1;
        else if (!strcmp(a, "--tokens")) o.show_tokens = 1;
        else if (!strcmp(a, "--rules")) o.rules_path = NEXT();
        else if (!strcmp(a, "--rules-dir")) o.rules_dir = NEXT();
        else if (!strcmp(a, "--lang")) o.lang_arg = NEXT();
        else if (!strcmp(a, "--passes")) o.passes_arg = NEXT();
        else if (!strcmp(a, "--simulate")) o.simulate = NEXT();
        else if (!strcmp(a, "--input")) o.sim_input = NEXT();
        else if (!strcmp(a, "--automaton")) o.sim_automaton = NEXT();
        else if (!strcmp(a, "--max-steps")) o.max_steps = atoll(NEXT());
        else if (!strcmp(a, "--max-tokens")) o.max_tokens = atoi(NEXT());
        else if (!strcmp(a, "--mode")) {
            const char *m = NEXT();
            o.scan_mode = !strcmp(m, "nfa") ? SCAN_NFA : !strcmp(m, "dfa") ? SCAN_DFA : SCAN_MIN;
        }
        else if (a[0] == '-') usage();
        else o.src_path = a;
#undef NEXT
    }
    return o;
}

static const char *lang_name(Lang l) { return l == LANG_C ? "C" : l == LANG_JAVA ? "Java" : "Python"; }

static int pick_lang(const Options *o, Lang *out, const char **reason) {
    const char *l = o->lang_arg;
    if (l && strcmp(l, "auto")) {
        if (!strcasecmp(l, "c")) *out = LANG_C;
        else if (!strcasecmp(l, "java")) *out = LANG_JAVA;
        else if (!strcasecmp(l, "python") || !strcasecmp(l, "py")) *out = LANG_PY;
        else return 0;
        *reason = "chosen explicitly";
        return 1;
    }
    const char *ext = o->src_path ? strrchr(o->src_path, '.') : NULL;
    if (ext && (!strcmp(ext, ".c") || !strcmp(ext, ".h"))) { *out = LANG_C; *reason = "file extension"; return 1; }
    if (ext && !strcmp(ext, ".java")) { *out = LANG_JAVA; *reason = "file extension"; return 1; }
    if (ext && !strcmp(ext, ".py")) { *out = LANG_PY; *reason = "file extension"; return 1; }
    return 0;
}

/* ---------------------------------------------------------------------
 * JSON: automata
 * ------------------------------------------------------------------- */

static void json_postfix(Buf *b, const Rule *r, const NFA *n) {
    char label[160], tmp[8];
    bputc(b, '[');
    for (int i = 0; i < r->pf.n; i++) {
        PfItem it = r->pf.v[i];
        if (i) bputc(b, ',');
        switch (it.k) {
        case PF_LIT: bputs(b, "{\"k\":\"lit\",\"v\":"); bjson_cstr(b, show_byte(it.v, tmp)); bputc(b, '}'); break;
        case PF_CLASS: cset_label(&n->cls.v[it.v], label, sizeof(label)); bputs(b, "{\"k\":\"class\",\"v\":"); bjson_cstr(b, label); bputc(b, '}'); break;
        case PF_EPS: bputs(b, "{\"k\":\"op\",\"v\":\"\xce\xb5\"}"); break;
        case PF_CAT: bputs(b, "{\"k\":\"op\",\"v\":\"\xc2\xb7\"}"); break;
        case PF_ALT: bputs(b, "{\"k\":\"op\",\"v\":\"|\"}"); break;
        case PF_STAR: bputs(b, "{\"k\":\"op\",\"v\":\"*\"}"); break;
        case PF_PLUS: bputs(b, "{\"k\":\"op\",\"v\":\"+\"}"); break;
        default: bputs(b, "{\"k\":\"op\",\"v\":\"?\"}"); break;
        }
    }
    bputc(b, ']');
}

static void json_nfa(Buf *b, const NFA *n, int start, const RuleSet *rs) {
    static const char *types[] = { "char", "class", "split", "match" };
    char label[160], tmp[8];
    bprintf(b, "{\"start\":%d,\"states\":[", start);
    for (int i = 0; i < n->s.n; i++) {
        const NState *s = &n->s.v[i];
        if (i) bputc(b, ',');
        bprintf(b, "{\"id\":%d,\"type\":\"%s\",\"out\":%d,\"out1\":%d,\"label\":", i, types[s->type], s->out, s->out1);
        if (s->type == NS_CHAR) bjson_cstr(b, show_byte(s->c, tmp));
        else if (s->type == NS_CLASS) { cset_label(&n->cls.v[s->c], label, sizeof(label)); bjson_cstr(b, label); }
        else if (s->type == NS_MATCH && rs) bjson_cstr(b, rs->r.v[s->rule].name);
        else bputs(b, "\"\"");
        if (s->type == NS_CHAR) bprintf(b, ",\"code\":%d", s->c);
        if (s->type == NS_CLASS) {
            bputs(b, ",\"ranges\":[");
            for (int c = 0, first = 1; c < 256; c++) {
                if (!cset_test(&n->cls.v[s->c], c)) continue;
                int lo = c;
                while (c + 1 < 256 && cset_test(&n->cls.v[s->c], c + 1)) c++;
                bprintf(b, "%s[%d,%d]", first ? "" : ",", lo, c);
                first = 0;
            }
            bputc(b, ']');
        }
        bputc(b, '}');
    }
    bputs(b, "]}");
}

/* Edges grouped per (from, to) with the union of their bytes as a label. */
static void json_dfa(Buf *b, const DFA *d, const RuleSet *rs, int max_states) {
    char label[160], tmp[8];
    int shown = d->nstates < max_states ? d->nstates : max_states;
    bprintf(b, "{\"start\":%d,\"total\":%d,\"symbols\":%d,\"states\":[", d->start, d->nstates, d->nsyms);
    for (int s = 0; s < shown; s++) {
        if (s) bputc(b, ',');
        bprintf(b, "{\"id\":%d,\"accept\":", s);
        if (d->accept[s] >= 0) bjson_cstr(b, rs->r.v[d->accept[s]].name); else bputs(b, "null");
        bputs(b, ",\"members\":[");
        for (int k = 0; k < d->members[s].n && k < 64; k++) bprintf(b, "%s%d", k ? "," : "", d->members[s].v[k]);
        if (d->members[s].n > 64) bputs(b, ",-1");
        bputs(b, "]}");
    }
    bputs(b, "],\"edges\":[");
    int first = 1;
    int *seen = xmalloc(sizeof(int) * (size_t)(d->nsyms ? d->nsyms : 1));
    for (int s = 0; s < shown; s++) {
        for (int a = 0; a < d->nsyms; a++) seen[a] = 0;
        for (int a = 0; a < d->nsyms; a++) {
            int t = d->trans[s * d->nsyms + a];
            if (t < 0 || seen[a] || t >= shown) continue;
            CSet set; memset(&set, 0, sizeof(set));
            int count = 0, single = -1;
            for (int a2 = a; a2 < d->nsyms; a2++) {
                if (d->trans[s * d->nsyms + a2] != t) continue;
                seen[a2] = 1;
                for (int c = 1; c < 256; c++) if (d->sym_of[c] == a2) { cset_add(&set, c); count++; single = c; }
            }
            if (count == 1) snprintf(label, sizeof(label), "%s", show_byte(single, tmp));
            else cset_label(&set, label, sizeof(label));
            bprintf(b, "%s{\"from\":%d,\"to\":%d,\"label\":", first ? "" : ",", s, t);
            bjson_cstr(b, label);
            bprintf(b, ",\"n\":%d}", count);
            first = 0;
        }
    }
    free(seen);
    bputs(b, "]}");
}

static void json_rules(Buf *b, RuleSet *rs) {
    bputs(b, "{\"name\":");
    bjson_cstr(b, rs->name);
    bputs(b, ",\"rules\":[");
    for (int i = 0; i < rs->r.n; i++) {
        Rule *r = &rs->r.v[i];
        if (i) bputc(b, ',');
        bputs(b, "{\"name\":"); bjson_cstr(b, r->name);
        bputs(b, ",\"pattern\":"); bjson_cstr(b, r->pattern);
        bprintf(b, ",\"priority\":%d,\"skip\":%s,\"neverWins\":%s,\"matchesEmpty\":%s,\"line\":%d",
                r->prio, r->skip ? "true" : "false", r->never_wins ? "true" : "false", r->matches_empty ? "true" : "false", r->line);
        bputs(b, ",\"postfix\":"); json_postfix(b, r, &r->nfa);
        bputs(b, ",\"nfa\":"); json_nfa(b, &r->nfa, r->start, NULL);
        /* this rule's own DFA and minimized DFA */
        RuleSet one;
        memset(&one, 0, sizeof(one));
        one.r = rs->r;
        DFA d, m;
        int prio0 = 0;
        if (dfa_build(&d, &r->nfa, r->start, &prio0, 4000) == 0) {
            dfa_minimize(&m, &d);
            /* accept indices refer to rule 'i' -- point the names at it */
            for (int s = 0; s < d.nstates; s++) if (d.accept[s] >= 0) d.accept[s] = i;
            for (int s = 0; s < m.nstates; s++) if (m.accept[s] >= 0) m.accept[s] = i;
            bputs(b, ",\"dfa\":"); json_dfa(b, &d, rs, 400);
            bputs(b, ",\"min\":"); json_dfa(b, &m, rs, 400);
            bprintf(b, ",\"minRounds\":%d", m.refine_rounds);
            dfa_free(&m);
        } else bputs(b, ",\"dfa\":null,\"min\":null");
        dfa_free(&d);
        bputc(b, '}');
    }
    bputs(b, "]");
    if (rs->dfa_ok) {
        bprintf(b, ",\"combined\":{\"nfaStates\":%d,\"dfaStates\":%d,\"minStates\":%d,\"dfaTransitions\":%d,\"minTransitions\":%d,"
                   "\"symbols\":%d,\"refineRounds\":%d,\"unreachableRemoved\":%d,\"buildMs\":{\"nfa\":%.3f,\"dfa\":%.3f,\"min\":%.3f}",
                rs->comb.s.n, rs->dfa.nstates, rs->min.nstates, dfa_transitions(&rs->dfa), dfa_transitions(&rs->min),
                rs->dfa.nsyms, rs->min.refine_rounds, rs->min.removed_unreachable, rs->ms_nfa, rs->ms_dfa, rs->ms_min);
        int nfa_edges = 0;
        for (int i = 0; i < rs->comb.s.n; i++) { const NState *s = &rs->comb.s.v[i]; nfa_edges += (s->out >= 0) + (s->out1 >= 0); }
        bprintf(b, ",\"nfaTransitions\":%d}", nfa_edges);
    }
    bputc(b, '}');
}

/* ---------------------------------------------------------------------
 * --simulate: one rule's automaton, step by step
 * ------------------------------------------------------------------- */

static int simulate(const Options *o, RuleSet *rs) {
    int ri = -1;
    for (int i = 0; i < rs->r.n; i++) if (!strcmp(rs->r.v[i].name, o->simulate)) ri = i;
    if (ri < 0) { fprintf(stderr, "Error: no rule named %s\n", o->simulate); return 1; }
    Rule *r = &rs->r.v[ri];
    const char *in = o->sim_input ? o->sim_input : "";
    Buf b = {0};
    char tmp[8];
    bprintf(&b, "{\"schema\":\"%s\",\"rule\":", SCHEMA);
    bjson_cstr(&b, r->name);
    bprintf(&b, ",\"automaton\":\"%s\",\"input\":", o->sim_automaton);
    bjson_cstr(&b, in);
    bputs(&b, ",\"steps\":[");
    int longest = -1;
    if (!strcmp(o->sim_automaton, "nfa")) {
        /* the set of active states after each character, and the epsilon closure that produced it */
        IntVec cur = {0}, moved = {0}, cl = {0};
        nfa_closure(&r->nfa, r->start, &cur);
        for (int i = 0; ; i++) {
            int acc = 0;
            for (int k = 0; k < cur.n; k++) acc |= r->nfa.s.v[cur.v[k]].type == NS_MATCH;
            if (acc) longest = i;
            bprintf(&b, "%s{\"pos\":%d,\"char\":", i ? "," : "", i);
            if (i == 0) bputs(&b, "null"); else bjson_cstr(&b, show_byte((unsigned char)in[i - 1], tmp));
            bputs(&b, ",\"moved\":[");
            for (int k = 0; k < moved.n; k++) bprintf(&b, "%s%d", k ? "," : "", moved.v[k]);
            bputs(&b, "],\"active\":[");
            for (int k = 0; k < cur.n; k++) bprintf(&b, "%s%d", k ? "," : "", cur.v[k]);
            bprintf(&b, "],\"accepting\":%s}", acc ? "true" : "false");
            if (!in[i] || !cur.n) break;
            moved.n = 0;
            for (int k = 0; k < cur.n; k++) {
                const NState *s = &r->nfa.s.v[cur.v[k]];
                int eats = s->type == NS_CHAR ? s->c == (unsigned char)in[i] : s->type == NS_CLASS ? cset_test(&r->nfa.cls.v[s->c], (unsigned char)in[i]) : 0;
                if (eats) vpush(moved, s->out);
            }
            /* union of the closures of the moved-to states */
            IntVec next = {0};
            char *seen = xcalloc((size_t)r->nfa.s.n, 1);
            for (int k = 0; k < moved.n; k++) {
                nfa_closure(&r->nfa, moved.v[k], &cl);
                for (int q = 0; q < cl.n; q++) if (!seen[cl.v[q]]) { seen[cl.v[q]] = 1; vpush(next, cl.v[q]); }
            }
            free(seen);
            vfree(cur);
            cur = next;
        }
        vfree(cur); vfree(moved); vfree(cl);
    } else {
        DFA d, m;
        int prio0 = 0;
        if (dfa_build(&d, &r->nfa, r->start, &prio0, 4000) < 0) { fprintf(stderr, "Error: DFA too large\n"); return 1; }
        dfa_minimize(&m, &d);
        DFA *use = !strcmp(o->sim_automaton, "min") ? &m : &d;
        int s = use->start;
        for (int i = 0; ; i++) {
            int acc = s >= 0 && use->accept[s] >= 0;
            if (acc) longest = i;
            bprintf(&b, "%s{\"pos\":%d,\"char\":", i ? "," : "", i);
            if (i == 0) bputs(&b, "null"); else bjson_cstr(&b, show_byte((unsigned char)in[i - 1], tmp));
            bprintf(&b, ",\"state\":%d,\"accepting\":%s}", s, acc ? "true" : "false");
            if (!in[i] || s < 0) break;
            s = use->trans[s * use->nsyms + use->sym_of[(unsigned char)in[i]]];
        }
        dfa_free(&d); dfa_free(&m);
    }
    int whole = (int)strlen(in);
    bprintf(&b, "],\"longestMatch\":%d,\"accepted\":%s}\n", longest, longest == whole && whole > 0 ? "true" : "false");
    fputs(b.s, stdout);
    free(b.s);
    return 0;
}

/* ---------------------------------------------------------------------
 * main pipeline
 * ------------------------------------------------------------------- */

typedef struct { const char *id, *name, *status, *summary; double ms; } Stage;

static void json_diags(Buf *b, Diags *ds) {
    bputc(b, '[');
    for (int i = 0; i < ds->d.n; i++) {
        Diag *d = &ds->d.v[i];
        if (i) bputc(b, ',');
        bprintf(b, "{\"severity\":\"%s\",\"stage\":\"%s\",\"category\":", d->sev == SEV_ERROR ? "error" : d->sev == SEV_WARNING ? "warning" : "note", d->stage);
        bjson_cstr(b, d->category);
        bprintf(b, ",\"line\":%d,\"col\":%d,\"message\":", d->line, d->col);
        bjson_cstr(b, d->msg);
        bputs(b, ",\"hint\":"); bjson_cstr(b, d->hint);
        bputc(b, '}');
    }
    bputc(b, ']');
}

static void json_tac(Buf *b, IRProg *p) {
    bputc(b, '[');
    for (int f = 0; f < p->f.n; f++) {
        Func *fn = &p->f.v[f];
        if (f) bputc(b, ',');
        bputs(b, "{\"func\":"); bjson_cstr(b, fn->name);
        bputs(b, ",\"params\":[");
        for (int i = 0; i < fn->params.n; i++) { if (i) bputc(b, ','); bjson_cstr(b, fn->params.v[i]); }
        bprintf(b, "],\"line\":%d,\"code\":[", fn->line);
        int first = 1;
        for (int i = 0; i < fn->code.n; i++) {
            Instr *in = &fn->code.v[i];
            if (in->op == I_NOP) continue;
            Buf t = {0};
            instr_str(&t, in, p->lang);
            bprintf(b, "%s{\"id\":%d,\"line\":%d,\"k\":\"%s\",\"t\":", first ? "" : ",", in->id, in->line,
                    in->op == I_LABEL ? "label" : in->op == I_OPAQUE ? "opaque" : (in->op == I_GOTO || in->op == I_IF || in->op == I_IFNOT) ? "jump" : in->op == I_CALL || in->op == I_PARAM ? "call" : "op");
            bjson_cstr(b, t.s);
            bputc(b, '}');
            free(t.s);
            first = 0;
        }
        bputs(b, "]}");
    }
    bputc(b, ']');
}

static void json_cfg(Buf *b, IRProg *p) {
    bputc(b, '[');
    for (int f = 0; f < p->f.n; f++) {
        Func *fn = &p->f.v[f];
        CFG g;
        cfg_build(fn, &g);
        IntVec be = {0};
        cfg_back_edges(&g, &be);
        if (f) bputc(b, ',');
        bputs(b, "{\"func\":"); bjson_cstr(b, fn->name);
        bputs(b, ",\"blocks\":[");
        for (int k = 0; k < g.n && k < 400; k++) {
            if (k) bputc(b, ',');
            bprintf(b, "{\"id\":%d,\"code\":[", k);
            for (int i = g.b[k].start; i < g.b[k].end; i++) {
                Buf t = {0};
                instr_str(&t, &fn->code.v[i], p->lang);
                if (i > g.b[k].start) bputc(b, ',');
                bjson_cstr(b, t.s);
                free(t.s);
            }
            bputs(b, "],\"succ\":[");
            for (int s = 0; s < g.b[k].succ.n; s++) bprintf(b, "%s%d", s ? "," : "", g.b[k].succ.v[s]);
            bputs(b, "]}");
        }
        bputs(b, "],\"backEdges\":[");
        for (int i = 0; i < be.n; i += 2) bprintf(b, "%s[%d,%d]", i ? "," : "", be.v[i], be.v[i + 1]);
        bputs(b, "]}");
        vfree(be);
        cfg_free(&g);
    }
    bputc(b, ']');
}

static void json_run(Buf *b, RunResult *r) {
    bprintf(b, "{\"status\":\"%s\",\"message\":", run_status_name(r->status));
    bjson_cstr(b, r->msg);
    bputs(b, ",\"stdout\":");
    bjson_str(b, r->out.s ? r->out.s : "", r->out.n > 200000 ? 200000 : r->out.n);
    bprintf(b, ",\"steps\":%lld,\"ms\":%.3f,\"exitCode\":%d,\"undefinedBehaviour\":%d,\"ubFirst\":", r->steps, r->ms, r->exit_code, r->ub_count);
    bjson_cstr(b, r->ub_count ? r->ub_first : NULL);
    bputc(b, '}');
}

/* time one scan mode: repeat until ~25 ms of work, report the per-run average */
static double time_scan(RuleSet *rs, const char *src, ScanMode mode, int *ntok) {
    double total = 0;
    int reps = 0;
    while (reps < 50 && (total < 25 || reps < 3)) {
        Tokens t = {0};
        double t0 = now_ms();
        lex(rs, src, mode, &t, NULL, 0);
        total += now_ms() - t0;
        *ntok = t.n;
        vfree(t);
        reps++;
        if (total > 2000) break;
    }
    return total / reps;
}

static long peak_rss_kb(void) {
#ifdef __EMSCRIPTEN__
    return (long)(__builtin_wasm_memory_size(0) * 64);   /* wasm pages are 64 KB */
#endif
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return ru.ru_maxrss / 1024;
#else
    return ru.ru_maxrss;
#endif
}

int main(int argc, char **argv) {
    Options o = parse_args(argc, argv);
    Diags ds = {0};
    ds.limit = 500;

    Lang lang = LANG_C;
    const char *lang_reason = "default";
    int have_lang = pick_lang(&o, &lang, &lang_reason);
    if (!have_lang && !o.rules_path && !o.simulate) {
        if (o.lang_arg && strcmp(o.lang_arg, "auto")) { fprintf(stderr, "Error: unknown language '%s'\n", o.lang_arg); return 2; }
        lang = LANG_C; lang_reason = "no extension; assumed C";
    }
    /* rules: explicit file, or the language's own fixed rule set */
    char rules_buf[512];
    const char *rules_path = o.rules_path;
    if (!rules_path) {
        snprintf(rules_buf, sizeof(rules_buf), "%s/%s.rules", o.rules_dir, lang == LANG_C ? "c" : lang == LANG_JAVA ? "java" : "python");
        rules_path = rules_buf;
    }
    int custom_rules = o.rules_path != NULL;
    long rlen;
    char *rules_text = read_file(rules_path, &rlen);
    if (!rules_text) { fprintf(stderr, "Error: cannot read rules file %s\n", rules_path); return 2; }
    RuleSet rs;
    double t_rules = now_ms();
    int rules_ok = rules_parse(&rs, rules_text, rules_path, &ds) == 0 && rules_build(&rs, &ds) == 0;
    t_rules = now_ms() - t_rules;

    if (o.simulate) {
        if (!rules_ok) { for (int i = 0; i < ds.d.n; i++) fprintf(stderr, "%s\n", ds.d.v[i].msg); return 1; }
        return simulate(&o, &rs);
    }

    long srclen = 0;
    char *src = o.src_path ? read_file(o.src_path, &srclen) : NULL;
    if (!src) { fprintf(stderr, "Error: cannot read %s\n", o.src_path ? o.src_path : "(no source file given)"); return 2; }
    if ((long)strlen(src) < srclen) diag(&ds, SEV_WARNING, "lexer", "NUL Byte", 0, 0, NULL, "the file contains a NUL byte; scanning stops there");
    int nlines = 1;
    for (long i = 0; i < srclen; i++) nlines += src[i] == '\n';

    VEC(Stage) stages = {0};
    Stage st;
    Tokens raw = {0}, toks = {0};
    Node *ast = NULL;
    IRProg ir, opt_ir;
    memset(&ir, 0, sizeof(ir)); memset(&opt_ir, 0, sizeof(opt_ir));
    LowerInfo li;
    memset(&li, 0, sizeof(li));
    OptReport rep;
    memset(&rep, 0, sizeof(rep));
    RunResult run0, run1;
    memset(&run0, 0, sizeof(run0)); memset(&run1, 0, sizeof(run1));
    Parser parser;
    memset(&parser, 0, sizeof(parser));
    int lex_errors = 0, ran = 0;
    int passes = o.passes_arg ? opt_parse_passes(o.passes_arg) : OPT_ALL;
    double t_lex = 0, t_parse = 0, t_tac = 0, t_opt = 0;
    double ms_nfa = 0, ms_dfa = 0, ms_min = 0;
    int ntok_nfa = 0, ntok_dfa = 0, ntok_min = 0, identical = 0;

    st = (Stage){ "rules", "Rules & Automata", rules_ok ? "ok" : "error", "", t_rules };
    vpush(stages, st);

    if (rules_ok) {
        double t0 = now_ms();
        lex_errors = lex(&rs, src, (ScanMode)o.scan_mode, &raw, &ds, 1);
        t_lex = now_ms() - t0;
        /* the three automata must agree token for token */
        Tokens a = {0}, b2 = {0}, c = {0};
        lex(&rs, src, SCAN_NFA, &a, NULL, 1);
        lex(&rs, src, SCAN_DFA, &b2, NULL, 1);
        lex(&rs, src, SCAN_MIN, &c, NULL, 1);
        identical = a.n == b2.n && b2.n == c.n;
        for (int i = 0; identical && i < a.n; i++)
            identical = a.v[i].rule == b2.v[i].rule && a.v[i].len == b2.v[i].len && a.v[i].pos == b2.v[i].pos &&
                        c.v[i].rule == a.v[i].rule && c.v[i].len == a.v[i].len && c.v[i].pos == a.v[i].pos;
        vfree(a); vfree(b2); vfree(c);
        ms_nfa = time_scan(&rs, src, SCAN_NFA, &ntok_nfa);
        ms_dfa = time_scan(&rs, src, SCAN_DFA, &ntok_dfa);
        ms_min = time_scan(&rs, src, SCAN_MIN, &ntok_min);
        if (custom_rules || o.lex_only) plain_layout(&raw, &toks);
        else if (lang == LANG_PY) python_layout(&raw, src, &toks, &ds);
        else plain_layout(&raw, &toks);
        int lex_diag_errors = diag_count_stage(&ds, "lexer", SEV_ERROR);
        char *sum = xmalloc(128);
        snprintf(sum, 128, "%d tokens, %d error%s", toks.n - 1, lex_diag_errors, lex_diag_errors == 1 ? "" : "s");
        st = (Stage){ "lex", "Lexical Analysis", lex_diag_errors ? "warn" : "ok", sum, t_lex };
        vpush(stages, st);
    } else {
        st = (Stage){ "lex", "Lexical Analysis", "skipped", "the rules did not compile", 0 };
        vpush(stages, st);
    }

    int full = rules_ok && !o.lex_only && !custom_rules;
    if (full) {
        parser.t = &toks; parser.ds = &ds; parser.lang = lang; parser.rs = &rs; parser.src = src;
        double t0 = now_ms();
        ast = parse_program(&parser);
        ast_fill_required(ast, lang == LANG_PY);
        t_parse = now_ms() - t0;
        int pe = diag_count_stage(&ds, "parser", SEV_ERROR);
        char *sum = xmalloc(128);
        snprintf(sum, 128, "%d AST nodes, %d syntax error%s", node_count(ast), pe, pe == 1 ? "" : "s");
        st = (Stage){ "parse", "Syntax Analysis", pe ? "error" : "ok", sum, t_parse };
        vpush(stages, st);

        t0 = now_ms();
        lower_program(ast, lang, &ir, &ds, &li);
        t_tac = now_ms() - t0;
        int se = diag_count_stage(&ds, "semantic", SEV_ERROR), sw = diag_count_stage(&ds, "semantic", SEV_WARNING);
        sum = xmalloc(128);
        snprintf(sum, 128, "%d symbols, %d error%s, %d warning%s", li.syms.n, se, se == 1 ? "" : "s", sw, sw == 1 ? "" : "s");
        st = (Stage){ "sema", "Semantic Analysis", se ? "error" : sw ? "warn" : "ok", sum, 0 };
        vpush(stages, st);
        sum = xmalloc(160);
        snprintf(sum, 160, "%d instructions in %d function%s%s", instr_count(&ir), ir.f.n, ir.f.n == 1 ? "" : "s",
                 li.opaque ? ", partial (opaque constructs)" : "");
        st = (Stage){ "tac", "Intermediate Code", li.opaque ? "warn" : "ok", sum, t_tac };
        vpush(stages, st);

        /* optimize a deep copy so the original stays available */
        opt_ir = ir;
        opt_ir.f.v = xmalloc(sizeof(Func) * (size_t)(ir.f.n ? ir.f.n : 1));
        opt_ir.f.cap = ir.f.n;
        for (int i = 0; i < ir.f.n; i++) {
            opt_ir.f.v[i] = ir.f.v[i];
            Code c = {0};
            for (int k = 0; k < ir.f.v[i].code.n; k++) vpush(c, ir.f.v[i].code.v[k]);
            opt_ir.f.v[i].code = c;
        }
        t0 = now_ms();
        optimize(&opt_ir, passes, &rep);
        t_opt = now_ms() - t0;
        int before = instr_count(&ir), after = instr_count(&opt_ir);
        sum = xmalloc(160);
        snprintf(sum, 160, "%d \xe2\x86\x92 %d instructions (%.1f%% fewer), %d change%s", before, after,
                 before ? 100.0 * (before - after) / before : 0.0, rep.rec.n, rep.rec.n == 1 ? "" : "s");
        st = (Stage){ "opt", "Optimization", "ok", sum, t_opt };
        vpush(stages, st);

        int compile_errors = diag_count_stage(&ds, "parser", SEV_ERROR) + diag_count_stage(&ds, "semantic", SEV_ERROR) +
                             diag_count_stage(&ds, "lexer", SEV_ERROR);
        if (!o.no_run && !compile_errors) {
            RunLimits lim = { o.max_steps, 20000, (size_t)256 << 20, (size_t)1 << 20 };
            ir_run(&ir, &lim, &run0);
            ir_run(&opt_ir, &lim, &run1);
            ran = 1;
            int same = run0.status == run1.status && run0.out.n == run1.out.n && (!run0.out.n || !memcmp(run0.out.s, run1.out.s, run0.out.n));
            sum = xmalloc(200);
            if (run0.status == RUN_UNSUPPORTED) snprintf(sum, 200, "not executable: %s", run0.msg);
            else snprintf(sum, 200, "%s; optimized output %s (%lld \xe2\x86\x92 %lld steps)", run0.status == RUN_OK ? "ran to completion" : run_status_name(run0.status),
                          same ? "identical" : "DIFFERS", run0.steps, run1.steps);
            st = (Stage){ "run", "Verification", run0.status == RUN_UNSUPPORTED ? "skipped" : same ? "ok" : "error", sum, run0.ms + run1.ms };
        } else st = (Stage){ "run", "Verification", "skipped", compile_errors ? "the program has compile errors" : "disabled (--no-run)", 0 };
        vpush(stages, st);
    } else if (rules_ok) {
        const char *why = custom_rules ? "custom lexical rules: lexical analysis only" : "lexical analysis only (--lex-only)";
        const char *ids[] = { "parse", "sema", "tac", "opt", "run" };
        const char *names[] = { "Syntax Analysis", "Semantic Analysis", "Intermediate Code", "Optimization", "Verification" };
        for (int i = 0; i < 5; i++) { st = (Stage){ ids[i], names[i], "skipped", why, 0 }; vpush(stages, st); }
    }

    /* ---------------- output ---------------- */
    Buf b = {0};
    if (o.json) {
        bprintf(&b, "{\"schema\":\"%s\",\"source\":{\"name\":", SCHEMA);
        bjson_cstr(&b, o.src_path);
        bprintf(&b, ",\"language\":\"%s\",\"languageReason\":", custom_rules ? "custom" : lang_name(lang));
        bjson_cstr(&b, lang_reason);
        bprintf(&b, ",\"bytes\":%ld,\"lines\":%d,\"customRules\":%s},", srclen, nlines, custom_rules ? "true" : "false");
        bputs(&b, "\"stages\":[");
        for (int i = 0; i < stages.n; i++) {
            Stage *s = &stages.v[i];
            bprintf(&b, "%s{\"id\":\"%s\",\"name\":\"%s\",\"status\":\"%s\",\"ms\":%.3f,\"summary\":", i ? "," : "", s->id, s->name, s->status, s->ms);
            bjson_cstr(&b, s->summary);
            bputc(&b, '}');
        }
        bputs(&b, "],\"rules\":");
        if (rules_ok) json_rules(&b, &rs); else bputs(&b, "null");
        /* tokens: [kind, rule, line, col, text] (skipped matches omitted) */
        bputs(&b, ",\"tokens\":{\"list\":[");
        int shown = 0, total = 0;
        for (int i = 0; i < toks.n; i++) {
            Token *t = &toks.v[i];
            if (t->kind == TK_EOF) continue;
            total++;
            if (shown >= o.max_tokens) continue;
            bprintf(&b, "%s[\"%s\",%d,%d,%d,", shown ? "," : "", t->rule >= 0 ? rs.r.v[t->rule].name : tok_kind_name(t->kind), t->rule, t->line, t->col);
            size_t tl = strlen(t->text);
            bjson_str(&b, t->text, tl > 200 ? 200 : tl);
            bputc(&b, ']');
            shown++;
        }
        bprintf(&b, "],\"count\":%d,\"shown\":%d,\"lexErrors\":%d}", total, shown, lex_errors);
        /* the scanner's path through the minimized DFA for the first tokens */
        bputs(&b, ",\"lexTrace\":[");
        if (rules_ok) {
            int k = 0;
            for (int i = 0; i < raw.n && k < 150; i++) {
                Token *t = &raw.v[i];
                if (t->kind == TK_SKIP && t->len > 0 && isspace((unsigned char)t->text[0])) continue;
                bprintf(&b, "%s{\"line\":%d,\"col\":%d,\"rule\":", k ? "," : "", t->line, t->col);
                bjson_cstr(&b, rs.r.v[t->rule].name);
                bputs(&b, ",\"text\":");
                bjson_str(&b, t->text, t->len > 60 ? 60 : (size_t)t->len);
                bprintf(&b, ",\"skip\":%s,\"path\":[%d", t->kind == TK_SKIP ? "true" : "false", rs.min.start);
                int s = rs.min.start;
                for (int c = 0; c < t->len && c < 60; c++) {
                    s = rs.min.trans[s * rs.min.nsyms + rs.min.sym_of[(unsigned char)t->text[c]]];
                    if (s < 0) break;
                    bprintf(&b, ",%d", s);
                }
                bputs(&b, "]}");
                k++;
            }
        }
        bputs(&b, "]");
        bprintf(&b, ",\"lexEquivalence\":{\"identical\":%s,\"nfa\":{\"tokens\":%d,\"ms\":%.4f},\"dfa\":{\"tokens\":%d,\"ms\":%.4f},\"min\":{\"tokens\":%d,\"ms\":%.4f}}",
                identical ? "true" : "false", ntok_nfa, ms_nfa, ntok_dfa, ms_dfa, ntok_min, ms_min);
        bputs(&b, ",\"ast\":");
        if (ast) { int budget = 6000; ast_json(&b, ast, &budget); } else bputs(&b, "null");
        bprintf(&b, ",\"astNodes\":%d", ast ? node_count(ast) : 0);
        bputs(&b, ",\"symbols\":[");
        for (int i = 0; i < li.syms.n && i < 3000; i++) {
            SymInfo *s = &li.syms.v[i];
            bputs(&b, i ? ",{\"name\":" : "{\"name\":"); bjson_cstr(&b, s->name);
            bputs(&b, ",\"kind\":"); bjson_cstr(&b, s->kind);
            bputs(&b, ",\"type\":"); bjson_cstr(&b, s->type);
            bputs(&b, ",\"scope\":"); bjson_cstr(&b, s->scope);
            bprintf(&b, ",\"line\":%d,\"uses\":%d}", s->line, s->uses);
        }
        bputs(&b, "]");
        if (full) {
            bputs(&b, ",\"tac\":{\"original\":"); json_tac(&b, &ir);
            bputs(&b, ",\"optimized\":"); json_tac(&b, &opt_ir);
            bprintf(&b, ",\"originalCount\":%d,\"optimizedCount\":%d,\"complete\":%s,\"opaque\":%d,\"externalCalls\":%d,\"entry\":[",
                    instr_count(&ir), instr_count(&opt_ir), li.complete ? "true" : "false", li.opaque, ir.external_calls);
            for (int i = 0; i < ir.entry.n; i++) { if (i) bputc(&b, ','); bjson_cstr(&b, ir.entry.v[i]); }
            bputs(&b, "]}");
            bputs(&b, ",\"opt\":{\"passes\":[");
            for (int i = 0; i < 11; i++) {
                int bit = 1 << i;
                bprintf(&b, "%s{\"key\":\"%s\",\"name\":\"%s\",\"enabled\":%s,\"count\":%d}", i ? "," : "", opt_pass_key(bit), opt_pass_name(bit),
                        (passes & bit) ? "true" : "false", rep.counts[i]);
            }
            bprintf(&b, "],\"rounds\":%d,\"ms\":%.3f,\"records\":[", rep.rounds, rep.ms);
            for (int i = 0; i < rep.rec.n; i++) {
                OptRecord *r = &rep.rec.v[i];
                bputs(&b, i ? ",{\"pass\":" : "{\"pass\":"); bjson_cstr(&b, r->pass);
                bputs(&b, ",\"func\":"); bjson_cstr(&b, r->func);
                bprintf(&b, ",\"line\":%d,\"block\":%d,\"id\":%d,\"before\":", r->line, r->block, r->id);
                bjson_cstr(&b, r->before);
                bputs(&b, ",\"after\":"); bjson_cstr(&b, r->after);
                bputs(&b, ",\"why\":"); bjson_cstr(&b, r->why);
                bputc(&b, '}');
            }
            bputs(&b, "]},\"cfg\":"); json_cfg(&b, &opt_ir);
            bputs(&b, ",\"cfgOriginal\":"); json_cfg(&b, &ir);
            bputs(&b, ",\"run\":");
            if (ran) {
                bputs(&b, "{\"original\":"); json_run(&b, &run0);
                bputs(&b, ",\"optimized\":"); json_run(&b, &run1);
                int same = run0.status == run1.status && run0.out.n == run1.out.n && (!run0.out.n || !memcmp(run0.out.s, run1.out.s, run0.out.n));
                bprintf(&b, ",\"equivalent\":%s}", run0.status == RUN_UNSUPPORTED ? "null" : same ? "true" : "false");
            } else bputs(&b, "null");
        }
        bputs(&b, ",\"trace\":{\"parser\":[");
        for (int i = 0; i < parser.trace.n; i++) { if (i) bputc(&b, ','); bjson_cstr(&b, parser.trace.v[i]); }
        bputs(&b, "],\"tac\":[");
        for (int i = 0; i < li.trace.n; i++) { if (i) bputc(&b, ','); bjson_cstr(&b, li.trace.v[i]); }
        bputs(&b, "]},\"diagnostics\":");
        json_diags(&b, &ds);
        bprintf(&b, ",\"diagnosticCounts\":{\"errors\":%d,\"warnings\":%d}", ds.errors, ds.warnings);
        bprintf(&b, ",\"metrics\":{\"lines\":%d,\"bytes\":%ld,\"tokens\":%d,\"astNodes\":%d,\"tacOriginal\":%d,\"tacOptimized\":%d,"
                    "\"ms\":{\"rules\":%.3f,\"lex\":%.3f,\"parse\":%.3f,\"tac\":%.3f,\"opt\":%.3f},\"peakMemoryKB\":%ld}}\n",
                nlines, srclen, toks.n ? toks.n - 1 : 0, ast ? node_count(ast) : 0, full ? instr_count(&ir) : 0, full ? instr_count(&opt_ir) : 0,
                t_rules, t_lex, t_parse, t_tac, t_opt, peak_rss_kb());
    } else {
        bprintf(&b, "Compiler Workbench -- %s (%s, %d lines)\n\n", o.src_path, custom_rules ? "custom rules" : lang_name(lang), nlines);
        if (rules_ok)
            bprintf(&b, "Automata (%s): NFA %d states -> DFA %d states -> minimized %d states, %d input symbols\n"
                        "  scan times: NFA %.3f ms, DFA %.3f ms, minimized DFA %.3f ms; token streams %s\n\n",
                    rs.name, rs.comb.s.n, rs.dfa.nstates, rs.min.nstates, rs.dfa.nsyms, ms_nfa, ms_dfa, ms_min, identical ? "identical" : "DIFFER");
        for (int i = 0; i < stages.n; i++) bprintf(&b, "  [%-7s] %-19s %s\n", stages.v[i].status, stages.v[i].name, stages.v[i].summary);
        bputc(&b, '\n');
        if (o.show_tokens) {
            for (int i = 0; i < toks.n; i++) {
                Token *t = &toks.v[i];
                bprintf(&b, "  %4d:%-4d %-10s '%.*s'\n", t->line, t->col, t->rule >= 0 ? rs.r.v[t->rule].name : tok_kind_name(t->kind), t->len > 60 ? 60 : t->len, t->text);
            }
            bputc(&b, '\n');
        }
        if (ds.d.n) {
            bputs(&b, "Diagnostics:\n");
            for (int i = 0; i < ds.d.n; i++) {
                Diag *d = &ds.d.v[i];
                bprintf(&b, "  %s:%d:%d: %s [%s] %s\n", o.src_path, d->line, d->col, d->sev == SEV_ERROR ? "error" : d->sev == SEV_WARNING ? "warning" : "note", d->category, d->msg);
                if (d->hint) bprintf(&b, "      hint: %s\n", d->hint);
            }
            bputc(&b, '\n');
        }
        if (full) {
            bputs(&b, "Three-address code:\n");
            ir_print(&b, &ir);
            bputs(&b, "Optimized:\n");
            ir_print(&b, &opt_ir);
            bprintf(&b, "Optimizations applied (%d):\n", rep.rec.n);
            for (int i = 0; i < rep.rec.n && i < 200; i++) {
                OptRecord *r = &rep.rec.v[i];
                bprintf(&b, "  [%s] %s line %d: %s  =>  %s\n      %s\n", r->pass, r->func, r->line, r->before ? r->before : "", r->after ? r->after : "(removed)", r->why);
            }
            if (ran) {
                bprintf(&b, "\nProgram output (original TAC, %s):\n%s", run_status_name(run0.status), run0.out.s ? run0.out.s : "");
                if (run0.msg[0]) bprintf(&b, "  [%s]\n", run0.msg);
                int same = run0.status == run1.status && run0.out.n == run1.out.n && (!run0.out.n || !memcmp(run0.out.s, run1.out.s, run0.out.n));
                bprintf(&b, "Optimized TAC output: %s (%lld vs %lld steps)\n", same ? "identical" : "DIFFERENT", run0.steps, run1.steps);
            }
        }
    }
    fwrite(b.s, 1, b.n, stdout);
    free(b.s);
    int errs = ds.errors;
    return errs ? 1 : 0;
}
