/* Unit tests for the regex parser, Thompson NFA, subset construction,
 * minimization and the scanner.   make test   (or ./build/test_automata) */
#include "lexer.h"

static int fails = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* longest non-empty match of one pattern via NFA, DFA and minimized DFA */
static void three_ways(const char *re, const char *text, int *nfa_len, int *dfa_len, int *min_len, int *nmin) {
    NFA n = {0};
    char err[200];
    int s = regex_compile(&n, re, 0, NULL, err, sizeof(err));
    NfaSim sim = {0};
    *nfa_len = nfa_longest(&sim, &n, s, text, NULL, NULL);
    DFA d, m;
    dfa_build(&d, &n, s, NULL, 10000);
    dfa_minimize(&m, &d);
    *dfa_len = dfa_longest(&d, text, NULL);
    *min_len = dfa_longest(&m, text, NULL);
    if (nmin) *nmin = m.nstates;
    dfa_free(&d); dfa_free(&m); nfa_free(&n); nfasim_free(&sim);
}

static void test_regex_errors(void) {
    struct { const char *re; const char *expect; } bad[] = {
        {"", "empty pattern"}, {"(ab", "no matching ')'"}, {"ab)", "no matching '('"}, {"*a", "nothing to repeat"},
        {"a|*b", "nothing to repeat"}, {"[abc", "never closed"}, {"ab\\", "lone backslash"}, {"[z-a]", "backwards range"},
        {"(((a)", "no matching ')'"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        NFA n = {0};
        char err[200] = "";
        int s = regex_compile(&n, bad[i].re, 0, NULL, err, sizeof(err));
        CHECK(s < 0, "'%s' should be rejected", bad[i].re);
        CHECK(strstr(err, bad[i].expect) != NULL, "'%s': message '%s' should mention '%s'", bad[i].re, err, bad[i].expect);
        nfa_free(&n);
    }
    const char *good[] = { "a|", "(|a)b", "()", "a**", "(a|b)*abb", "[^a-c]x", ".", "\\.", "[]a]", "[a\\-z]", "((a))", "a?b+c*" };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        NFA n = {0};
        char err[200] = "";
        CHECK(regex_compile(&n, good[i], 0, NULL, err, sizeof(err)) >= 0, "'%s' should compile: %s", good[i], err);
        nfa_free(&n);
    }
}

static void test_matching(void) {
    struct { const char *re, *text; int len; } t[] = {
        {"a", "a", 1}, {"a", "b", 0}, {"ab|a", "ab", 2}, {"(a|b)*abb", "babbx", 4}, {"(a|b)*abb", "ab", 0},
        {"[0-9]+", "123abc", 3}, {"[0-9]+(\\.[0-9]+)?", "3.14x", 4}, {"[0-9]+(\\.[0-9]+)?", "3.x", 1},
        {"a?b", "b", 1}, {"a?b", "ab", 2}, {"x*", "xxxy", 3}, {"x*", "y", 0}, {"[^\"]*\"", "abc\"d", 4},
        {".+", "ab\ncd", 2}, {"\\*\\+", "*+", 2}, {"(|a)b", "b", 1}, {"(|a)b", "ab", 2}, {"[]a]+", "]a]b", 3},
        {"/\\*([^*]|\\*+[^*/])*\\*+/", "/* a ** b */ x", 12}, {"if|[a-z]+", "iffy", 4},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        int a, b, c;
        three_ways(t[i].re, t[i].text, &a, &b, &c, NULL);
        CHECK(a == t[i].len && b == t[i].len && c == t[i].len, "'%s' on '%s': NFA %d DFA %d min %d, expected %d", t[i].re, t[i].text, a, b, c, t[i].len);
    }
}

/* Exhaustive equivalence over every string up to length 7 on {a, b, c}. */
static void test_equivalence(void) {
    const char *res[] = { "(a|b)*abb", "a(b|c)*a|c+", "((a|b)(a|b))*", "(a*b*)*c?", "a+b+|b+a+", "(ab|ba)*(c|)", "[ab]*c[ab]*" };
    char s[8];
    for (size_t r = 0; r < sizeof(res) / sizeof(res[0]); r++) {
        NFA n = {0};
        char err[100];
        int st = regex_compile(&n, res[r], 0, NULL, err, sizeof(err));
        DFA d, m;
        dfa_build(&d, &n, st, NULL, 10000);
        dfa_minimize(&m, &d);
        NfaSim sim = {0};
        int mismatches = 0, total = 0;
        for (int len = 1; len <= 7; len++) {
            int count = 1;
            for (int i = 0; i < len; i++) count *= 3;
            for (int x = 0; x < count; x++) {
                int y = x;
                for (int i = 0; i < len; i++) { s[i] = (char)('a' + y % 3); y /= 3; }
                s[len] = 0;
                int a = nfa_longest(&sim, &n, st, s, NULL, NULL), b = dfa_longest(&d, s, NULL), c = dfa_longest(&m, s, NULL);
                total++;
                if (a != b || b != c) mismatches++;
            }
        }
        CHECK(mismatches == 0, "'%s': %d of %d strings disagree between NFA, DFA and minimized DFA", res[r], mismatches, total);
        CHECK(m.nstates <= d.nstates, "'%s': minimized DFA is larger", res[r]);
        dfa_free(&d); dfa_free(&m); nfa_free(&n); nfasim_free(&sim);
    }
    /* the textbook example: (a|b)*abb has a 4-state minimal DFA */
    int a, b, c, nmin;
    three_ways("(a|b)*abb", "abb", &a, &b, &c, &nmin);
    CHECK(nmin == 4, "(a|b)*abb minimal DFA has %d states, expected 4", nmin);
}

static void test_closure(void) {
    NFA n = {0};
    char err[100];
    int s = regex_compile(&n, "a*b", 0, NULL, err, sizeof(err));
    IntVec cl = {0};
    nfa_closure(&n, s, &cl);
    int chars = 0;
    for (int i = 0; i < cl.n; i++) chars += n.s.v[cl.v[i]].type == NS_CHAR;
    CHECK(chars == 2, "closure of a*b's start should reach both 'a' and 'b' states, got %d char states", chars);
    vfree(cl);
    nfa_free(&n);
}

static int scan(const char *rules, const char *src, ScanMode mode, Tokens *out, Diags *ds) {
    RuleSet rs;
    Diags rd = {0};
    if (rules_parse(&rs, rules, "test", &rd) < 0 || rules_build(&rs, &rd) < 0) return -1;
    int e = lex(&rs, src, mode, out, ds, 0);
    for (int i = 0; i < out->n; i++) out->v[i].rule = out->v[i].rule;   /* indices stay valid after free */
    rules_free(&rs);
    return e;
}

static void test_scanner(void) {
    const char *rules =
        "KEYWORD 1 token if|else|while\n"
        "IDENT   2 token [a-z_][a-z0-9_]*\n"
        "NUMBER  2 token [0-9]+\n"
        "OP      2 token <=|<|=|==|\\+\\+|\\+\n"
        "WS      3 skip  [ \\t\\n]+\n";
    for (int mode = 0; mode < 3; mode++) {
        Tokens t = {0};
        Diags ds = {0};
        int errs = scan(rules, "if iffy <= x++ == 10\n  else @ y", (ScanMode)mode, &t, &ds);
        const char *want[] = { "if", "iffy", "<=", "x", "++", "==", "10", "else", "y" };
        int wrule[] = { 0, 1, 3, 1, 3, 3, 2, 0, 1 };
        CHECK(t.n == 9, "mode %d: %d tokens, expected 9", mode, t.n);
        for (int i = 0; i < t.n && i < 9; i++)
            CHECK(!strcmp(t.v[i].text, want[i]) && t.v[i].rule == wrule[i], "mode %d token %d: '%s' rule %d", mode, i, t.v[i].text, t.v[i].rule);
        CHECK(errs == 1 && ds.d.n == 1 && ds.d.v[0].line == 2 && ds.d.v[0].col == 8, "mode %d: '@' should be one error at 2:8", mode);
        if (t.n == 9) CHECK(t.v[7].line == 2 && t.v[7].col == 3, "mode %d: 'else' at %d:%d, expected 2:3", mode, t.v[7].line, t.v[7].col);
        vfree(t);
    }
    /* a rule that can never win is reported */
    RuleSet rs;
    Diags ds = {0};
    rules_parse(&rs, "ID 1 token [a-z]+\nKW 2 token if\n", "t", &ds);
    rules_build(&rs, &ds);
    int warned = 0;
    for (int i = 0; i < ds.d.n; i++) warned |= strstr(ds.d.v[i].category, "Unreachable") != NULL;
    CHECK(warned, "KW after a higher-priority [a-z]+ should be reported as unreachable");
    rules_free(&rs);
    /* malformed rule lines */
    Diags d2 = {0};
    CHECK(rules_parse(&rs, "JUSTANAME\n", "t", &d2) < 0, "a rule without priority/mode should be rejected");
}

static void test_python_layout(void) {
    long n;
    char *rules = read_file("rules/python.rules", &n);
    if (!rules) { printf("skip: rules/python.rules not found (run from the project root)\n"); return; }
    RuleSet rs;
    Diags ds = {0};
    rules_parse(&rs, rules, "python", &ds);
    rules_build(&rs, &ds);
    const char *src = "def f(x):\n    if x:\n        return [1,\n  2]\n    return 0\nprint(f(1))\n";
    Tokens raw = {0}, t = {0};
    lex(&rs, src, SCAN_MIN, &raw, &ds, 1);
    python_layout(&raw, src, &t, &ds);
    int ind = 0, ded = 0, nl = 0;
    for (int i = 0; i < t.n; i++) { ind += t.v[i].kind == TK_INDENT; ded += t.v[i].kind == TK_DEDENT; nl += t.v[i].kind == TK_NEWLINE; }
    CHECK(ind == 2 && ded == 2, "INDENT %d DEDENT %d, expected 2 and 2", ind, ded);
    CHECK(nl == 5, "NEWLINE %d, expected 5 (the bracketed line continues)", nl);
    CHECK(ds.errors == 0, "no layout errors expected, got %d", ds.errors);
    Diags d2 = {0};
    Tokens r2 = {0}, t2 = {0};
    const char *bad = "if x:\n    a\n  b\n";
    lex(&rs, bad, SCAN_MIN, &r2, &d2, 1);
    python_layout(&r2, bad, &t2, &d2);
    CHECK(d2.errors == 1, "inconsistent dedent should be one error, got %d", d2.errors);
    rules_free(&rs);
    free(rules);
}

int main(void) {
    test_regex_errors();
    test_matching();
    test_equivalence();
    test_closure();
    test_scanner();
    test_python_layout();
    printf("automata tests: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
