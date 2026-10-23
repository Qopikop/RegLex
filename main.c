#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>


#define MAX_STATES   1024
#define MAX_CLASSES  120     /* placeholders are 128+idx, so must stay < 128 */
#define MAX_RULES    32
#define MAX_PATTERN  4096    /* longest regex (after expansion and in postfix) */

typedef enum { CHAR, CLASS, SPLIT, MATCH } StateType;

typedef struct State State;
struct State {
    StateType type;
    int c;              /* literal char (CHAR) or class index (CLASS) */
    State *out, *out1;
    int id;
};

typedef struct Ptrlist Ptrlist;
struct Ptrlist { State **p; Ptrlist *next; };

typedef struct { State *start; Ptrlist *out; } Frag;

typedef struct { State *s[MAX_STATES]; int n; } StatePool;

typedef struct { unsigned char bits[32]; } CharClass;   /* 256-bit set */

static CharClass classes[MAX_CLASSES];
static int nclasses = 0;

static void die(const char *msg) {
    fprintf(stderr, "Error: %s\n", msg);
    exit(1);
}

/* ---- char class bitset helpers ---- */

static void cc_set(unsigned char *bm, unsigned char c) { bm[c >> 3] |= (unsigned char)(1u << (c & 7)); }
static int  cc_test(const unsigned char *bm, unsigned char c) { return (bm[c >> 3] >> (c & 7)) & 1; }

/* Parses one [...] starting at re[*i] == '['. Fills classes[nclasses],
 * advances *i past the closing ']', and returns the new class index. */
static int parse_class(const char *re, int *i) {
    if (nclasses >= MAX_CLASSES) die("too many character classes in one lexer");
    int idx = nclasses++;
    unsigned char *bm = classes[idx].bits;
    memset(bm, 0, 32);

    int j = *i + 1;              /* skip '[' */
    int negate = 0;
    if (re[j] == '^') { negate = 1; j++; }

    int first = 1;
    while (re[j] != '\0' && (re[j] != ']' || first)) {
        first = 0;
        unsigned char lo = (unsigned char)re[j];
        if (lo == '\\' && re[j + 1]) { j++; lo = (unsigned char)re[j]; }

        if (re[j + 1] == '-' && re[j + 2] && re[j + 2] != ']') {
            unsigned char hi = (unsigned char)re[j + 2];
            if (hi < lo) die("backwards range in character class");
            for (int c = lo; c <= hi; c++) cc_set(bm, (unsigned char)c);
            j += 3;
        } else {
            cc_set(bm, lo);
            j += 1;
        }
    }
    if (re[j] != ']') die("unterminated character class");
    j++;

    if (negate) {
        for (int b = 0; b < 32; b++) bm[b] = (unsigned char)(~bm[b]);
    }
    *i = j;
    return idx;
}

/* Replaces every [...] in `re` with a single placeholder byte so the
 * rest of the pipeline can treat it like any other literal atom.
 * Placeholders live in 128..191, so a real byte >= 128 in the pattern
 * (e.g. UTF-8) is escaped here to keep it from being read as a class. */
static void expand_classes(const char *re, char *out) {
    int oi = 0;
    for (int i = 0; re[i] != '\0'; ) {
        if (oi + 2 >= MAX_PATTERN) die("regular expression too long");
        if (re[i] == '[') {
            int idx = parse_class(re, &i);
            out[oi++] = (char)(128 + idx);
        } else if ((unsigned char)re[i] >= 128) {
            out[oi++] = '\\';
            out[oi++] = re[i++];
        } else if (re[i] == '\\' && re[i + 1]) {
            out[oi++] = re[i++];
            out[oi++] = re[i++];
        } else {
            out[oi++] = re[i++];
        }
    }
    out[oi] = '\0';
}

/* ---- NFA fragment plumbing ---- */

/* Ptrlists are only needed while one regex is being compiled, so they come
 * from a fixed arena that compile_regex resets -- nothing to malloc or free.
 * Every list1() call goes with a new_state(), so MAX_STATES is enough. */
static Ptrlist ptrlist_arena[MAX_STATES];
static int nptrlists = 0;

static Ptrlist *list1(State **p) {
    if (nptrlists >= MAX_STATES) die("too many NFA states -- pattern too large");
    Ptrlist *l = &ptrlist_arena[nptrlists++];
    l->p = p; l->next = NULL;
    return l;
}
static Ptrlist *append(Ptrlist *a, Ptrlist *b) { if (!a) return b; Ptrlist *t = a; while (t->next) t = t->next; t->next = b; return a; }
static void patch(Ptrlist *l, State *s) { while (l) { *(l->p) = s; l = l->next; } }

static State *new_state(StatePool *p, StateType type, int c, State *a, State *b) {
    if (p->n >= MAX_STATES) die("too many NFA states -- pattern too large");
    State *s = calloc(1, sizeof(*s));
    s->type = type; s->c = c; s->out = a; s->out1 = b; s->id = p->n;
    p->s[p->n++] = s;
    return s;
}

static void free_pool(StatePool *p) {
    for (int i = 0; i < p->n; i++) free(p->s[i]);
    p->n = 0;
}

/* ---- infix -> postfix ---- */

/* Internal concat operator. A non-printable byte, so a literal '.' in a
 * pattern can no longer be mistaken for concatenation. */
#define CONCAT '\x01'

static int precedence(unsigned char c) { return c == '|' ? 1 : (c == CONCAT ? 2 : 3); }

/* Pops operators of equal/higher precedence (stopping at '('), then
 * pushes an implicit concatenation. */
static void push_concat(char *ops, int *no, char *out, int *nout) {
    while (*no && ops[*no - 1] != '(' && precedence((unsigned char)ops[*no - 1]) >= 2) out[(*nout)++] = ops[--(*no)];
    ops[(*no)++] = CONCAT;
}

static int to_postfix(const char *re, char *out) {
    char ops[128];
    int no = 0, nout = 0, need_concat = 0;

    for (int i = 0; re[i]; i++) {
        unsigned char c = (unsigned char)re[i];
        /* one step adds at most 3 bytes across ops+out (escape: CONCAT + '\' + char),
         * and everything left on ops ends up in out, so bound the sum */
        if (no + 3 > (int)sizeof(ops) || nout + no + 3 >= MAX_PATTERN)
            die("regular expression too long or too deeply nested");

        if (c == '\\' && re[i + 1]) {
            if (need_concat) push_concat(ops, &no, out, &nout);
            /* keep the backslash so compile_regex knows the next byte is a literal */
            out[nout++] = '\\';
            out[nout++] = re[++i];
            need_concat = 1;
        } else if (c == '(') {
            if (need_concat) push_concat(ops, &no, out, &nout);
            ops[no++] = (char)c;
            need_concat = 0;
        } else if (c == ')') {
            while (no && ops[no - 1] != '(') out[nout++] = ops[--no];
            if (!no) die("unmatched parenthesis");
            no--;
            need_concat = 1;
        } else if (c == '|') {
            while (no && ops[no - 1] != '(' && precedence((unsigned char)ops[no - 1]) >= 1) out[nout++] = ops[--no];
            ops[no++] = (char)c;
            need_concat = 0;
        } else if (c == '*' || c == '+' || c == '?') {
            out[nout++] = (char)c;
            need_concat = 1;
        } else {
            if (need_concat) push_concat(ops, &no, out, &nout);
            out[nout++] = (char)c;
            need_concat = 1;
        }
    }
    while (no) { if (ops[no - 1] == '(') die("unmatched parenthesis"); out[nout++] = ops[--no]; }
    out[nout] = '\0';
    return nout;
}

/* ---- postfix -> NFA (compile_regex now also emits CLASS states) ---- */

static State *compile_regex(const char *raw, StatePool *pool) {
    if (strlen(raw) >= MAX_PATTERN) die("regular expression too long");
    nptrlists = 0;
    char expanded[MAX_PATTERN];
    expand_classes(raw, expanded);

    char post[MAX_PATTERN];
    to_postfix(expanded, post);

    Frag stack[128];
    int ns = 0;

    for (int i = 0; post[i]; i++) {
        unsigned char c = (unsigned char)post[i];
        if (ns >= (int)(sizeof(stack) / sizeof(stack[0]))) die("regular expression too deeply nested");
        if (c == '\\' && post[i + 1]) {
            /* escaped byte: always a literal, never an operator */
            State *s = new_state(pool, CHAR, (unsigned char)post[++i], NULL, NULL);
            stack[ns++] = (Frag){ s, list1(&s->out) };
        } else if (c == CONCAT) {
            if (ns < 2) die("bad concatenation");
            Frag e2 = stack[--ns], e1 = stack[--ns];
            patch(e1.out, e2.start);
            stack[ns++] = (Frag){ e1.start, e2.out };
        } else if (c == '|') {
            if (ns < 2) die("bad alternation");
            Frag e2 = stack[--ns], e1 = stack[--ns];
            State *s = new_state(pool, SPLIT, 0, e1.start, e2.start);
            stack[ns++] = (Frag){ s, append(e1.out, e2.out) };
        } else if (c == '*') {
            if (ns < 1) die("bad star");
            Frag e = stack[--ns];
            State *s = new_state(pool, SPLIT, 0, e.start, NULL);
            patch(e.out, s);
            stack[ns++] = (Frag){ s, list1(&s->out1) };
        } else if (c == '+') {
            if (ns < 1) die("bad plus");
            Frag e = stack[--ns];
            State *s = new_state(pool, SPLIT, 0, e.start, NULL);
            patch(e.out, s);
            stack[ns++] = (Frag){ e.start, list1(&s->out1) };
        } else if (c == '?') {
            if (ns < 1) die("bad question mark");
            Frag e = stack[--ns];
            State *s = new_state(pool, SPLIT, 0, e.start, NULL);
            stack[ns++] = (Frag){ s, append(e.out, list1(&s->out1)) };
        } else if (c >= 128) {
            State *s = new_state(pool, CLASS, c - 128, NULL, NULL);
            stack[ns++] = (Frag){ s, list1(&s->out) };
        } else {
            State *s = new_state(pool, CHAR, c, NULL, NULL);
            stack[ns++] = (Frag){ s, list1(&s->out) };
        }
    }
    if (ns != 1) die("invalid regular expression");

    State *m = new_state(pool, MATCH, 0, NULL, NULL);
    patch(stack[0].out, m);
    return stack[0].start;
}

/* ---- NFA simulation ---- */

typedef struct { State *v[MAX_STATES]; int n; } StateSet;

/* seen[id] == mark means "already in the set being built". Bumping the
 * mark per step avoids clearing the whole array for every character. */
static unsigned seen[MAX_STATES];
static unsigned mark = 0;

static void addstate(StateSet *l, State *s) {
    if (!s || seen[s->id] == mark) return;
    seen[s->id] = mark;
    if (s->type == SPLIT) {
        addstate(l, s->out);
        addstate(l, s->out1);
    } else {
        l->v[l->n++] = s;
    }
}

static void next_mark(void) {
    if (++mark == 0) { memset(seen, 0, sizeof(seen)); mark = 1; }   /* wrapped around */
}

/* Printable form of one input byte for error messages: \n, \t, \r, or \xNN. */
static const char *show_char(unsigned char c) {
    static char buf[8];
    if (c == '\n') return "\\n";
    if (c == '\t') return "\\t";
    if (c == '\r') return "\\r";
    if (isprint(c)) snprintf(buf, sizeof(buf), "%c", c);
    else            snprintf(buf, sizeof(buf), "\\x%02x", c);
    return buf;
}

static int state_eats(State *s, unsigned char ch) {
    if (s->type == CHAR)  return s->c == ch;
    if (s->type == CLASS) return cc_test(classes[s->c].bits, ch);
    return 0;
}

/* Longest-prefix match: returns the length of the longest prefix of
 * `text` accepted by the NFA rooted at `start`, or -1 if no prefix
 * (not even the empty one) is accepted. This is what a real scanner
 * needs -- Stage 1's match() only asked yes/no about the whole string. */
static int longest_match(State *start, const char *text) {
    /* two sets swapped by pointer each step -- no 8KB struct copies */
    static StateSet a, b;
    StateSet *cur = &a, *next = &b;
    cur->n = 0;
    next_mark();
    addstate(cur, start);

    int best = -1;
    for (int j = 0; j < cur->n; j++) if (cur->v[j]->type == MATCH) { best = 0; break; }

    int i = 0;
    while (text[i] != '\0' && cur->n > 0) {
        next->n = 0;
        next_mark();
        for (int j = 0; j < cur->n; j++) {
            if (state_eats(cur->v[j], (unsigned char)text[i]))
                addstate(next, cur->v[j]->out);
        }
        i++;
        StateSet *t = cur; cur = next; next = t;
        for (int j = 0; j < cur->n; j++) if (cur->v[j]->type == MATCH) { best = i; break; }
    }
    return best;
}

/* ---- token rule table ---- */

typedef struct {
    const char *name;
    const char *pattern;
    int priority;   /* lower wins ties -- keywords beat IDENT this way */
    int skip;       /* 1 = matched but not emitted (whitespace, comments...) */
    State *start;
    StatePool pool;
} Rule;

static Rule rules[MAX_RULES];
static int nrules = 0;

static void add_rule(const char *name, const char *pattern, int priority, int skip) {
    if (nrules >= MAX_RULES) die("too many token rules");
    Rule *r = &rules[nrules++];
    r->name = name; r->pattern = pattern; r->priority = priority; r->skip = skip;
    r->pool = (StatePool){0};
    r->start = compile_regex(pattern, &r->pool);
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

/* Scans `text` against every rule using maximal munch: at each position
 * try all rules, keep the longest match, break length ties by priority.
 * On no match at all, report the bad character and skip past it so the
 * rest of the line still gets scanned (basic error recovery). */
static void scan(const char *text) {
    int pos = 0, line = 1, col = 1;
    while (text[pos] != '\0') {
        int best_len = 0, best_rule = -1;

        for (int r = 0; r < nrules; r++) {
            int len = longest_match(rules[r].start, text + pos);
            if (len <= 0) continue;
            if (len > best_len || (len == best_len && rules[r].priority < rules[best_rule].priority))
                { best_len = len; best_rule = r; }
        }

        if (best_rule == -1) {
            printf("  line %d col %d: unrecognized character '%s' -- skipped\n", line, col, show_char((unsigned char)text[pos]));
            if (text[pos] == '\n') { line++; col = 1; } else col++;
            pos++;
            continue;
        }

        if (!rules[best_rule].skip) {
            printf("  %4d:%-4d %-10s '", line, col, rules[best_rule].name);
            print_lexeme(text + pos, best_len);
            printf("'\n");
        }
        for (int k = 0; k < best_len; k++) {
            if (text[pos + k] == '\n') { line++; col = 1; } else col++;
        }
        pos += best_len;
    }
}


/* Part 1 demo inputs and the Part 2 rule setup live here (not inside main)
 * so visualize/trace_json.c can reuse exactly the same definitions. */
static const char *demo_patterns[] = { "[a-z]", "[0-9]+", "[a-zA-Z_][a-zA-Z0-9_]*" };
static const char *demo_tests[]    = { "a", "7", "123", "hello", "abc123", "A1", "_tmp9" };
#define N_DEMO_PATTERNS 3
#define N_DEMO_TESTS    7

/* Built-in general rules: the tokens most languages share (C, C++, Java,
 * JavaScript, Python...). Where languages disagree -- '#' is a comment in
 * Python but a directive in C, '//' is a comment in C but division in
 * Python -- load a language's own file instead: --rules rules/python.rules */
static void setup_rules(void) {
    add_rule("KEYWORD",
        "auto|break|case|char|const|continue|default|do|double|else|enum|extern|float|for|goto|if|"
        "inline|int|long|register|return|short|signed|sizeof|static|struct|switch|typedef|union|"
        "unsigned|void|volatile|while|"
        "bool|boolean|byte|catch|class|delete|extends|final|finally|implements|import|interface|"
        "namespace|new|null|nullptr|package|private|protected|public|super|template|this|throw|"
        "throws|true|false|try|using|virtual|abstract|"
        "and|as|assert|async|await|def|del|elif|except|from|global|in|is|lambda|None|nonlocal|not|"
        "or|pass|raise|self|True|False|with|yield|"
        "function|let|var|typeof|instanceof|undefined|export|of",                       1, 0);
    add_rule("DIRECTIVE",
        "#[ \t]*(include|define|undef|ifdef|ifndef|if|elif|else|endif|pragma|error|warning|line)[^\n]*",
                                                                                         1, 0);
    add_rule("COMMENT",   "//[^\n]*|/\\*([^*]|\\*+[^*/])*\\*+/|#[^\n]*",                 2, 1);
    add_rule("STRING",    "\"([^\"\\\\\n]|\\\\[^\n])*\"|'([^'\\\\\n]|\\\\[^\n])*'|`[^`]*`", 2, 0);
    add_rule("NUMBER",
        "0[xX][0-9a-fA-F]+[uUlL]*|0[bB][01]+|"
        "[0-9]+(\\.[0-9]*)?([eE][+\\-]?[0-9]+)?[uUlLfFjJ]*|\\.[0-9]+([eE][+\\-]?[0-9]+)?[fFlLjJ]?", 2, 0);
    add_rule("IDENT",     "[a-zA-Z_$][a-zA-Z0-9_$]*",                                    2, 0);
    add_rule("OP",
        ">>>=|<<=|>>=|\\*\\*=|\\?\\?=|===|!==|>>>|\\?\\?|\\?\\.|\\.\\.\\.|->|=>|::|:=|\\+\\+|--|&&|\\|\\||==|!=|<=|>=|"
        "<<|>>|\\+=|-=|\\*=|/=|%=|&=|\\|=|\\^=|\\*\\*|[-+*/%=<>!&|^~?@]",              2, 0);
    add_rule("PUNCT",     "[(){}\\[\\];,.:]",                                            2, 0);
    add_rule("WS",        "[ \t\r\n\f\v]+",                                              3, 1);
}

/* Rules files let one lexer handle any language. One rule per line:
 *     NAME  PRIORITY  token|skip  REGEX
 * Blank lines and lines starting with '#' are ignored. In REGEX, \n \t \r
 * \f \v mean those characters; every other \x stays a regex escape. */
static char *unescape_rule(const char *s) {
    char *out = malloc(strlen(s) + 1);
    int k = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '\\' && s[i + 1]) {
            const char *from = "ntrfv", *to = "\n\t\r\f\v", *hit = strchr(from, s[i + 1]);
            if (hit) { out[k++] = to[hit - from]; i++; continue; }
            out[k++] = s[i++];            /* keep the backslash and the escaped char */
        }
        out[k++] = s[i];
    }
    out[k] = '\0';
    return out;
}

static void load_rules(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) { perror(path); exit(1); }
    char line[MAX_PATTERN];
    int lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        line[strcspn(line, "\r\n")] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;

        char name[64], mode[16];
        int prio, used = 0;
        if (sscanf(p, "%63s %d %15s %n", name, &prio, mode, &used) != 3 || used == 0 || p[used] == '\0'
            || (strcmp(mode, "token") != 0 && strcmp(mode, "skip") != 0)) {
            fprintf(stderr, "Error: %s line %d: expected  NAME PRIORITY token|skip REGEX\n", path, lineno);
            exit(1);
        }
        add_rule(strdup(name), unescape_rule(p + used), prio, strcmp(mode, "skip") == 0);
    }
    fclose(fp);
    if (nrules == 0) die("rules file contains no rules");
}

/* Reads a whole file into a NUL-terminated heap buffer, any size. */
static char *read_file(const char *path, long *len_out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); exit(1); }
    size_t cap = 4096, n = 0, got;
    char *buf = malloc(cap);
    while ((got = fread(buf + n, 1, cap - n - 1, fp)) > 0) {
        n += got;
        if (n + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
    }
    if (ferror(fp)) { perror(path); exit(1); }
    fclose(fp);
    buf[n] = '\0';
    if (strlen(buf) < n)
        fprintf(stderr, "Warning: %s contains a NUL byte -- scanning stops there\n", path);
    *len_out = (long)n;
    return buf;
}

/* Command line:  [source-file] [--rules file]
 * Loads the rules and returns the source path (default: source.txt). */
static const char *configure(int argc, char **argv, const char **rules_path) {
    const char *src = "source.txt";
    *rules_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rules") == 0 && i + 1 < argc) *rules_path = argv[++i];
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: %s [source-file] [--rules rules-file]\n", argv[0]);
            exit(1);
        } else src = argv[i];
    }
    if (*rules_path) load_rules(*rules_path); else setup_rules();
    return src;
}

int main(int argc, char **argv) {
    const char *rules_path;
    const char *src_path = configure(argc, argv, &rules_path);

    printf("Regex Lexer Generator\n");
    printf("=====================\n\n");

    /* --- Part 1: direct character classes, no more manual expansion ---
     * Stage 1 had to spell "[a-z]" out as (a|b|c|...|z) by hand in the
     * driver code. That's gone now; the patterns below are compiled as
     * written. */
    printf("Part 1 -- character classes compiled directly\n\n");
    for (int p = 0; p < N_DEMO_PATTERNS; p++) {
        StatePool pool = {0};
        State *s = compile_regex(demo_patterns[p], &pool);
        printf("Pattern %-24s NFA states: %d\n", demo_patterns[p], pool.n);
        for (int t = 0; t < N_DEMO_TESTS; t++) {
            int len = longest_match(s, demo_tests[t]);
            int whole = (len == (int)strlen(demo_tests[t]));
            printf("  %-8s %s\n", demo_tests[t], (len > 0 && whole) ? "MATCH" : "reject");
        }
        free_pool(&pool);
        puts("");
    }

    /* --- Part 2: token rule table + longest-match scanner over a whole file --- */
    printf("Part 2 -- scanning %s with %d rules (%s)\n\n", src_path, nrules,
           rules_path ? rules_path : "built-in general rules");

    long n;
    char *source = read_file(src_path, &n);
    if (n <= 1000) printf("input:\n%s\n", source);
    else           printf("input: %ld bytes (not echoed)\n\n", n);
    printf("tokens:\n");
    scan(source);
    puts("");

    printf("Interactive view of this run:  python3 visualize/build.py %s%s%s --open\n",
           src_path, rules_path ? " --rules " : "", rules_path ? rules_path : "");

    free(source);
    for (int r = 0; r < nrules; r++) free_pool(&rules[r].pool);
    return 0;
}
