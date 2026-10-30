#include "lexer.h"
#include <ctype.h>

/* In rules files, \n \t \r \f \v mean those characters; every other \x
 * stays a regex escape (the regex parser reads it as a literal x). */
static char *unescape_rule(const char *s) {
    char *out = xmalloc(strlen(s) + 1);
    int k = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '\\' && s[i + 1]) {
            const char *from = "ntrfv", *to = "\n\t\r\f\v", *hit = strchr(from, s[i + 1]);
            if (hit) { out[k++] = to[hit - from]; i++; continue; }
            out[k++] = s[i++];
        }
        out[k++] = s[i];
    }
    out[k] = '\0';
    return out;
}

int rules_parse(RuleSet *rs, const char *text, const char *name, Diags *ds) {
    memset(rs, 0, sizeof(*rs));
    rs->name = intern(name);
    int lineno = 0, bad = 0;
    for (const char *p = text; *p; ) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char *line = xstrndup(p, len);
        p += len + (eol ? 1 : 0);
        lineno++;
        line[strcspn(line, "\r")] = '\0';
        char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '\0' || *q == '#') { free(line); continue; }

        char nm[64], mode[16];
        int prio, used = 0;
        if (sscanf(q, "%63s %d %15s %n", nm, &prio, mode, &used) != 3 || used == 0 || q[used] == '\0'
            || (strcmp(mode, "token") != 0 && strcmp(mode, "skip") != 0)) {
            diag(ds, SEV_ERROR, "rules", "Malformed Rule", lineno, 1,
                 "Write each rule as:  NAME  PRIORITY  token|skip  REGEX",
                 "%s line %d is not a valid rule", name, lineno);
            bad = 1; free(line); continue;
        }
        for (int i = 0; i < rs->r.n; i++)
            if (strcmp(rs->r.v[i].name, nm) == 0)
                diag(ds, SEV_WARNING, "rules", "Duplicate Rule", lineno, 1, "Give each rule a distinct name.",
                     "rule %s is defined twice (lines %d and %d)", nm, rs->r.v[i].line, lineno);
        Rule r = {0};
        r.name = xstrdup(nm);
        r.pattern = unescape_rule(q + used);
        r.prio = prio;
        r.skip = strcmp(mode, "skip") == 0;
        r.line = lineno;
        vpush(rs->r, r);
        free(line);
    }
    if (!rs->r.n && !bad)
        diag(ds, SEV_ERROR, "rules", "Empty Rule Set", 0, 0, NULL, "%s contains no rules", name);
    return (bad || !rs->r.n) ? -1 : 0;
}

int rules_build(RuleSet *rs, Diags *ds) {
    char err[256];
    int bad = 0;
    double t0 = now_ms();
    rs->prio = xmalloc(sizeof(int) * (size_t)(rs->r.n ? rs->r.n : 1));
    VEC(int) starts = {0};
    for (int i = 0; i < rs->r.n; i++) {
        Rule *r = &rs->r.v[i];
        rs->prio[i] = r->prio;
        r->start = regex_compile(&r->nfa, r->pattern, i, &r->pf, err, sizeof(err));
        if (r->start < 0) {
            diag(ds, SEV_ERROR, "rules", "Invalid Regex", r->line, 1, NULL, "rule %s: %s", r->name, err);
            bad = 1;
            continue;
        }
        int s = regex_compile(&rs->comb, r->pattern, i, NULL, err, sizeof(err));
        vpush(starts, s);
        IntVec cl = {0};
        nfa_closure(&r->nfa, r->start, &cl);
        for (int k = 0; k < cl.n; k++) if (r->nfa.s.v[cl.v[k]].type == NS_MATCH) r->matches_empty = 1;
        vfree(cl);
        if (r->matches_empty)
            diag(ds, SEV_WARNING, "rules", "Empty Match", r->line, 1, "Use + instead of * so the rule must consume input.",
                 "rule %s can match the empty string; empty matches are never used as tokens", r->name);
    }
    if (bad) { vfree(starts); return -1; }

    /* one start state with an epsilon edge to every rule's fragment */
    int start = -1;
    for (int i = starts.n - 1; i >= 0; i--) {
        if (start < 0) { start = starts.v[i]; continue; }
        NState sp = { NS_SPLIT, 0, starts.v[i], start, -1 };
        vpush(rs->comb.s, sp);
        start = rs->comb.s.n - 1;
    }
    rs->comb_start = start;
    vfree(starts);
    double t1 = now_ms();
    rs->ms_nfa = t1 - t0;

    if (dfa_build(&rs->dfa, &rs->comb, rs->comb_start, rs->prio, MAX_DFA_STATES) < 0) {
        diag(ds, SEV_ERROR, "rules", "DFA Too Large", 0, 0, "Simplify the patterns, or scan with the NFA.",
             "subset construction needed more than %d states", MAX_DFA_STATES);
        rs->ms_dfa = now_ms() - t1;
        return -1;
    }
    double t2 = now_ms();
    rs->ms_dfa = t2 - t1;
    dfa_minimize(&rs->min, &rs->dfa);
    rs->ms_min = now_ms() - t2;
    rs->dfa_ok = 1;

    /* rules no DFA state accepts can never produce a token */
    char *wins = xcalloc((size_t)rs->r.n, 1);
    for (int s = 0; s < rs->dfa.nstates; s++) if (rs->dfa.accept[s] >= 0) wins[rs->dfa.accept[s]] = 1;
    for (int i = 0; i < rs->r.n; i++) {
        Rule *r = &rs->r.v[i];
        if (wins[i]) continue;
        r->never_wins = 1;
        if (!r->matches_empty || r->nfa.s.n > 2)
            diag(ds, SEV_WARNING, "rules", "Unreachable Rule", r->line, 1,
                 "Give it a lower priority number, or move it before the rules that shadow it.",
                 "rule %s never wins: every string it matches is claimed by a higher-priority rule", r->name);
    }
    free(wins);
    return 0;
}

void rules_free(RuleSet *rs) {
    for (int i = 0; i < rs->r.n; i++) {
        free(rs->r.v[i].name); free(rs->r.v[i].pattern);
        nfa_free(&rs->r.v[i].nfa); vfree(rs->r.v[i].pf);
    }
    vfree(rs->r);
    nfa_free(&rs->comb);
    free(rs->prio);
    if (rs->dfa_ok) { dfa_free(&rs->dfa); dfa_free(&rs->min); }
}

static TokKind kind_for_rule(const char *name) {
    static const struct { const char *n; TokKind k; } map[] = {
        {"KEYWORD", TK_KEYWORD}, {"IDENT", TK_IDENT}, {"IDENTIFIER", TK_IDENT}, {"NUMBER", TK_NUMBER},
        {"INT", TK_NUMBER}, {"FLOAT", TK_NUMBER}, {"STRING", TK_STRING}, {"CHAR", TK_CHAR},
        {"OP", TK_OP}, {"OPERATOR", TK_OP}, {"PUNCT", TK_PUNCT}, {"DIRECTIVE", TK_DIRECTIVE},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) if (strcmp(map[i].n, name) == 0) return map[i].k;
    return TK_OTHER;
}

const char *tok_kind_name(TokKind k) {
    static const char *n[] = { "KEYWORD", "IDENT", "NUMBER", "STRING", "CHAR", "OP", "PUNCT", "DIRECTIVE",
                               "OTHER", "NEWLINE", "INDENT", "DEDENT", "EOF", "SKIP" };
    return n[k];
}

int lex(RuleSet *rs, const char *src, ScanMode mode, Tokens *out, Diags *ds, int keep_skipped) {
    NfaSim sim = {0};
    int pos = 0, line = 1, col = 1, errors = 0;
    while (src[pos]) {
        int rule = -1, len;
        if (mode == SCAN_NFA)      len = nfa_longest(&sim, &rs->comb, rs->comb_start, src + pos, rs->prio, &rule);
        else if (mode == SCAN_DFA) len = dfa_longest(&rs->dfa, src + pos, &rule);
        else                       len = dfa_longest(&rs->min, src + pos, &rule);

        if (len == 0) {
            /* recovery: report the character (a whole UTF-8 sequence) and skip it */
            unsigned char c = (unsigned char)src[pos];
            int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            for (int k = 1; k < n; k++) if (!src[pos + k]) { n = k; break; }
            char shown[16], tmp[8];
            if (n > 1) snprintf(shown, sizeof(shown), "%.*s", n, src + pos);
            else snprintf(shown, sizeof(shown), "%s", show_byte(c, tmp));
            if (ds) diag(ds, SEV_ERROR, "lexer", "Lexical Error", line, col,
                         "Remove the character, or add a rule that matches it.",
                         "unrecognized character '%s' -- skipped", shown);
            errors++;
            if (src[pos] == '\n') { line++; col = 1; } else col++;
            pos += n;
            continue;
        }
        Rule *r = &rs->r.v[rule];
        if (!r->skip || keep_skipped) {
            Token t = { r->skip ? TK_SKIP : kind_for_rule(r->name), rule, pos, len, line, col, intern_n(src + pos, (size_t)len) };
            vpush(*out, t);
        }
        for (int k = 0; k < len; k++) {
            if (src[pos + k] == '\n') { line++; col = 1; }
            else if (((unsigned char)src[pos + k] & 0xC0) != 0x80) col++;   /* count characters, not bytes */
        }
        pos += len;
    }
    nfasim_free(&sim);
    return errors;
}

void plain_layout(const Tokens *in, Tokens *out) {
    int line = 1, col = 1;
    for (int i = 0; i < in->n; i++) {
        if (in->v[i].kind == TK_SKIP) continue;
        vpush(*out, in->v[i]);
        line = in->v[i].line; col = in->v[i].col + in->v[i].len;
    }
    Token eof = { TK_EOF, -1, 0, 0, line, col, intern("<EOF>") };
    vpush(*out, eof);
}

/* ---- Python layout ---- */

static int end_line(const Token *t) {
    int l = t->line;
    for (int k = 0; k < t->len; k++) if (t->text[k] == '\n') l++;
    return l;
}

/* visual indentation of the line `t` starts (tabs to the next multiple of 8) */
static int indent_of(const char *src, const Token *t) {
    int p = t->pos;
    while (p > 0 && src[p - 1] != '\n') p--;
    int w = 0;
    for (; p < t->pos; p++) w = src[p] == '\t' ? (w / 8 + 1) * 8 : w + 1;
    return w;
}

static int starts_statement(const Token *t) {
    static const char *kw[] = { "def", "class", "if", "elif", "else", "for", "while", "return", "import", "from",
                                "try", "except", "finally", "with", "raise", "pass", NULL };
    if (t->kind != TK_KEYWORD) return 0;
    for (int i = 0; kw[i]; i++) if (strcmp(t->text, kw[i]) == 0) return 1;
    return 0;
}

void python_layout(const Tokens *in, const char *src, Tokens *out, Diags *ds) {
    int stack[256], top = 0, depth = 0;
    stack[0] = 0;
    const Token *prev = NULL, *opener = NULL;   /* outermost unclosed bracket */
    for (int i = 0; i < in->n; i++) {
        const Token *t = &in->v[i];
        if (t->kind == TK_SKIP) continue;
        /* recovery: a statement keyword starting a line no further right than the
           line that opened the bracket means the bracket was never closed */
        if (prev && depth > 0 && opener && t->line > end_line(prev) && starts_statement(t) &&
            indent_of(src, t) <= indent_of(src, opener)) {
            diag(ds, SEV_ERROR, "lexer", "Syntax Error", opener->line, opener->col, "Add the missing closing bracket.",
                 "'%s' opened on line %d was never closed", opener->text, opener->line);
            depth = 0;
            opener = NULL;
        }
        if (!prev) {
            if (indent_of(src, t) > 0)
                diag(ds, SEV_ERROR, "lexer", "Indentation Error", t->line, t->col, "Remove the leading spaces.",
                     "unexpected indent on the first line");
        } else if (t->line > end_line(prev) && depth == 0) {
            /* a backslash at the end of the previous line joins the lines */
            int joined = 0;
            for (int p = prev->pos + prev->len; p < t->pos; p++) if (src[p] == '\\' && src[p + 1] == '\n') joined = 1;
            if (!joined) {
                Token nl = { TK_NEWLINE, -1, prev->pos + prev->len, 0, end_line(prev), prev->col + prev->len, intern("NEWLINE") };
                vpush(*out, nl);
                int w = indent_of(src, t);
                if (w > stack[top]) {
                    if (top < 255) stack[++top] = w;
                    Token ind = { TK_INDENT, -1, t->pos, 0, t->line, 1, intern("INDENT") };
                    vpush(*out, ind);
                } else {
                    while (w < stack[top]) {
                        top--;
                        Token ded = { TK_DEDENT, -1, t->pos, 0, t->line, 1, intern("DEDENT") };
                        vpush(*out, ded);
                    }
                    if (w != stack[top])
                        diag(ds, SEV_ERROR, "lexer", "Indentation Error", t->line, 1,
                             "Indent this line to match an enclosing block.",
                             "unindent does not match any outer indentation level");
                }
            }
        }
        if (t->len == 1 && strchr("([{", t->text[0])) { if (!depth) opener = t; depth++; }
        if (t->len == 1 && strchr(")]}", t->text[0]) && depth > 0) depth--;
        vpush(*out, *t);
        prev = t;
    }
    if (depth > 0 && opener)
        diag(ds, SEV_ERROR, "lexer", "Syntax Error", opener->line, opener->col, "Add the missing closing bracket.",
             "'%s' opened on line %d was never closed", opener->text, opener->line);
    int line = prev ? end_line(prev) : 1;
    if (prev) { Token nl = { TK_NEWLINE, -1, prev->pos + prev->len, 0, line, 0, intern("NEWLINE") }; vpush(*out, nl); }
    while (top > 0) { top--; Token ded = { TK_DEDENT, -1, 0, 0, line + 1, 1, intern("DEDENT") }; vpush(*out, ded); }
    Token eof = { TK_EOF, -1, 0, 0, line + 1, 1, intern("<EOF>") };
    vpush(*out, eof);
}
