/* The original lexer command line, now running on the shared engine in src/
 * (regex -> postfix -> Thompson NFA -> DFA -> minimized DFA). The output
 * format is unchanged:
 *
 *   ./lexer [source-file] [--rules rules-file]
 *
 * Without --rules the built-in general rules (rules/general.rules, compiled
 * into the program) are used; without a file name, source.txt is scanned.
 * The full compiler -- parser, TAC, optimizer -- is build/rcc.
 */
#include "src/lexer.h"
#include "build/general_rules.h"

static const char *demo_patterns[] = { "[a-z]", "[0-9]+", "[a-zA-Z_][a-zA-Z0-9_]*" };
static const char *demo_tests[]    = { "a", "7", "123", "hello", "abc123", "A1", "_tmp9" };
#define N_DEMO_PATTERNS 3
#define N_DEMO_TESTS    7

static const char *show_char(unsigned char c) {
    static char buf[8];
    if (c == '\n') return "\\n";
    if (c == '\t') return "\\t";
    if (c == '\r') return "\\r";
    if (c >= 32 && c < 127) snprintf(buf, sizeof(buf), "%c", c);
    else snprintf(buf, sizeof(buf), "\\x%02x", c);
    return buf;
}

/* Prints a token's text on one line: newlines/tabs escaped, long ones cut. */
static void print_lexeme(const char *s, int len) {
    int shown = len > 60 ? 57 : len;
    for (int i = 0; i < shown; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n' || c == '\t' || c == '\r') fputs(show_char(c), stdout);
        else putchar(c);
    }
    if (shown < len) printf("... (%d chars)", len);
}

int main(int argc, char **argv) {
    const char *src_path = "source.txt", *rules_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rules") == 0 && i + 1 < argc) rules_path = argv[++i];
        else if (argv[i][0] == '-') { fprintf(stderr, "usage: %s [source-file] [--rules rules-file]\n", argv[0]); return 1; }
        else src_path = argv[i];
    }

    long n;
    char *rules_text = rules_path ? read_file(rules_path, &n) : xstrndup((const char *)rules_general_rules, rules_general_rules_len);
    if (!rules_text) { perror(rules_path); return 1; }
    RuleSet rs;
    Diags ds = {0};
    if (rules_parse(&rs, rules_text, rules_path ? rules_path : "built-in general rules", &ds) < 0 || rules_build(&rs, &ds) < 0) {
        for (int i = 0; i < ds.d.n; i++) if (ds.d.v[i].sev == SEV_ERROR) fprintf(stderr, "Error: %s\n", ds.d.v[i].msg);
        return 1;
    }

    printf("Regex Lexer Generator\n");
    printf("=====================\n\n");

    /* Part 1: single patterns, each compiled to its own NFA */
    printf("Part 1 -- character classes compiled directly\n\n");
    NfaSim sim = {0};
    for (int p = 0; p < N_DEMO_PATTERNS; p++) {
        NFA nfa = {0};
        char err[200];
        int start = regex_compile(&nfa, demo_patterns[p], 0, NULL, err, sizeof(err));
        printf("Pattern %-24s NFA states: %d\n", demo_patterns[p], nfa.s.n);
        for (int t = 0; t < N_DEMO_TESTS; t++) {
            int len = nfa_longest(&sim, &nfa, start, demo_tests[t], NULL, NULL);
            printf("  %-8s %s\n", demo_tests[t], (len > 0 && len == (int)strlen(demo_tests[t])) ? "MATCH" : "reject");
        }
        nfa_free(&nfa);
        puts("");
    }
    nfasim_free(&sim);

    /* Part 2: the rule table scans a whole file with maximal munch (minimized DFA) */
    printf("Part 2 -- scanning %s with %d rules (%s)\n\n", src_path, rs.r.n, rules_path ? rules_path : "built-in general rules");
    char *source = read_file(src_path, &n);
    if (!source) { perror(src_path); return 1; }
    if ((long)strlen(source) < n) fprintf(stderr, "Warning: %s contains a NUL byte -- scanning stops there\n", src_path);
    if (n <= 1000) printf("input:\n%s\n", source);
    else           printf("input: %ld bytes (not echoed)\n\n", n);
    printf("tokens:\n");

    Tokens toks = {0};
    Diags lexd = {0};
    lex(&rs, source, SCAN_MIN, &toks, &lexd, 0);
    /* tokens and lexical errors, in source order */
    int e = 0;
    for (int i = 0; i <= toks.n; i++) {
        while (e < lexd.d.n && (i == toks.n || lexd.d.v[e].line < toks.v[i].line ||
                                (lexd.d.v[e].line == toks.v[i].line && lexd.d.v[e].col < toks.v[i].col))) {
            const char *m = strchr(lexd.d.v[e].msg, '\'');
            const char *q = m ? strrchr(m + 1, '\'') : NULL;
            printf("  line %d col %d: unrecognized character %.*s -- skipped\n", lexd.d.v[e].line, lexd.d.v[e].col,
                   m && q ? (int)(q - m + 1) : 3, m ? m : "'?'");
            e++;
        }
        if (i == toks.n) break;
        Token *t = &toks.v[i];
        printf("  %4d:%-4d %-10s '", t->line, t->col, rs.r.v[t->rule].name);
        print_lexeme(t->text, t->len);
        printf("'\n");
    }
    puts("");
    printf("Interactive view:  python3 visualize/serve.py  (the compiler workbench)\n");
    printf("Full compiler (parser, TAC, optimizer):  build/rcc <file.c|.java|.py>\n");
    free(source);
    vfree(toks);
    rules_free(&rs);
    free(rules_text);
    return 0;
}
