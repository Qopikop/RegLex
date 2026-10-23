/* Dumps everything the lexer does as JSON, for the web visualizer.
 *
 * It #includes main.c so it runs the exact same regex compiler, NFA
 * simulation and rule table -- nothing is reimplemented here except the
 * outer scan loop, which is repeated with recording added.
 *
 * Normally run through build.py (python3 visualize/build.py), which
 * compiles this, runs it on source.txt and embeds the JSON in the page.
 */

#define main lexer_main
#include "../main.c"
#undef main

static void json_str(const char *s, int len) {
    putchar('"');
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c == '\n') printf("\\n");
        else if (c == '\t') printf("\\t");
        else if (c == '\r') printf("\\r");
        else if (c < 32 || c >= 127) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static void json_cstr(const char *s) { json_str(s, (int)strlen(s)); }

/* Rebuilds a readable [..] from a class bitset, e.g. "[a-zA-Z_]". */
static void class_label(int idx, char *buf) {
    const unsigned char *bm = classes[idx].bits;
    int count = 0;
    for (int c = 0; c < 256; c++) count += cc_test(bm, (unsigned char)c);
    int inv = count > 128;                 /* mostly-full sets read better as [^...] */
#define IN(c) (cc_test(bm, (unsigned char)(c)) ^ inv)
    int k = 0;
    buf[k++] = '[';
    if (inv) buf[k++] = '^';
    for (int c = 0; c < 256; ) {
        if (!IN(c)) { c++; continue; }
        int lo = c;
        while (c + 1 < 256 && IN(c + 1)) c++;
        int hi = c++;
        if (k > 40) { buf[k++] = '.'; buf[k++] = '.'; break; }
        const char *e;
        for (int x = lo; x <= hi; x += (hi - lo >= 2 ? hi - lo : 1)) {
            e = show_char((unsigned char)x);
            if (x && strchr("-]^\\", x)) buf[k++] = '\\';   /* keep a lone '-' from reading as a range */
            k += snprintf(buf + k, 128 - k, "%s", e);   /* callers pass a 128-byte buffer */
            if (hi - lo >= 2 && x == lo) buf[k++] = '-';
        }
    }
    buf[k++] = ']';
    buf[k] = '\0';
#undef IN
}

/* Postfix form of a pattern as a list of {k: kind, v: text} items. */
static void json_postfix(const char *raw) {
    int saved = nclasses;          /* expand_classes allocates fresh classes; give them back */
    char expanded[MAX_PATTERN], post[MAX_PATTERN], label[128];
    expand_classes(raw, expanded);
    to_postfix(expanded, post);
    printf("[");
    for (int i = 0; post[i]; i++) {
        unsigned char c = (unsigned char)post[i];
        if (i) printf(",");
        if (c == '\\' && post[i + 1]) {
            char lit[2] = { post[++i], 0 };
            printf("{\"k\":\"lit\",\"v\":"); json_cstr(lit); printf("}");
        } else if (c == CONCAT) {
            printf("{\"k\":\"op\",\"v\":\"\\u00b7\"}");
        } else if (c == '|' || c == '*' || c == '+' || c == '?') {
            printf("{\"k\":\"op\",\"v\":\"%c\"}", c);
        } else if (c >= 128) {
            class_label(c - 128, label);
            printf("{\"k\":\"class\",\"v\":"); json_cstr(label); printf("}");
        } else {
            char lit[2] = { (char)c, 0 };
            printf("{\"k\":\"lit\",\"v\":"); json_cstr(lit); printf("}");
        }
    }
    printf("]");
    nclasses = saved;
}

static void json_nfa(StatePool *p, State *start) {
    static const char *types[] = { "char", "class", "split", "match" };
    char label[128];
    printf("{\"start\":%d,\"states\":[", start->id);
    for (int i = 0; i < p->n; i++) {
        State *s = p->s[i];
        if (i) printf(",");
        printf("{\"id\":%d,\"type\":\"%s\",\"out\":%d,\"out1\":%d,\"label\":",
               s->id, types[s->type], s->out ? s->out->id : -1, s->out1 ? s->out1->id : -1);
        if (s->type == CHAR) { json_cstr(show_char((unsigned char)s->c)); }
        else if (s->type == CLASS) { class_label(s->c, label); json_cstr(label); }
        else printf("\"\"");
        /* exact input each state accepts, so the NFA page can run strings through it */
        if (s->type == CHAR) printf(",\"code\":%d", (unsigned char)s->c);
        if (s->type == CLASS) {
            const unsigned char *bm = classes[s->c].bits;
            printf(",\"ranges\":[");
            for (int c = 0, first = 1; c < 256; c++) {
                if (!cc_test(bm, (unsigned char)c)) continue;
                int lo = c;
                while (c + 1 < 256 && cc_test(bm, (unsigned char)(c + 1))) c++;
                printf("%s[%d,%d]", first ? "" : ",", lo, c);
                first = 0;
            }
            printf("]");
        }
        printf("}");
    }
    printf("]}");
}

int main(int argc, char **argv) {
    const char *rules_path;
    const char *path = configure(argc, argv, &rules_path);   /* same arguments as ./lexer */
    long n;
    char *source = read_file(path, &n);

    printf("{\"sourceName\":"); json_cstr(path);
    printf(",\"rulesSource\":"); json_cstr(rules_path ? rules_path : "built-in general rules");
    printf(",\"source\":"); json_str(source, (int)n);

    /* Part 1: standalone patterns against test strings */
    printf(",\"demo\":[");
    for (int p = 0; p < N_DEMO_PATTERNS; p++) {
        StatePool pool = {0};
        State *s = compile_regex(demo_patterns[p], &pool);
        if (p) printf(",");
        printf("{\"pattern\":"); json_cstr(demo_patterns[p]);
        printf(",\"postfix\":"); json_postfix(demo_patterns[p]);
        printf(",\"nfa\":"); json_nfa(&pool, s);
        printf(",\"results\":[");
        for (int t = 0; t < N_DEMO_TESTS; t++) {
            int len = longest_match(s, demo_tests[t]);
            int whole = (len == (int)strlen(demo_tests[t]));
            printf("%s{\"input\":", t ? "," : ""); json_cstr(demo_tests[t]);
            printf(",\"len\":%d,\"match\":%s}", len, (len > 0 && whole) ? "true" : "false");
        }
        printf("]}");
        free_pool(&pool);
    }
    printf("]");

    /* Part 2: rule table (loaded by configure) */
    printf(",\"rules\":[");
    for (int r = 0; r < nrules; r++) {
        if (r) printf(",");
        printf("{\"name\":"); json_cstr(rules[r].name);
        printf(",\"pattern\":"); json_cstr(rules[r].pattern);
        printf(",\"priority\":%d,\"skip\":%s", rules[r].priority, rules[r].skip ? "true" : "false");
        printf(",\"postfix\":"); json_postfix(rules[r].pattern);
        printf(",\"nfa\":"); json_nfa(&rules[r].pool, rules[r].start);
        printf("}");
    }
    printf("]");

    /* Part 2: the scan, one step per decision (same logic as scan()) */
    printf(",\"steps\":[");
    int pos = 0, line = 1, col = 1, first = 1;
    while (source[pos] != '\0') {
        int best_len = 0, best_rule = -1;
        printf("%s{\"pos\":%d,\"line\":%d,\"col\":%d,\"cands\":[", first ? "" : ",", pos, line, col);
        first = 0;
        for (int r = 0; r < nrules; r++) {
            int len = longest_match(rules[r].start, source + pos);
            printf("%s%d", r ? "," : "", len);
            if (len <= 0) continue;
            if (len > best_len || (len == best_len && rules[r].priority < rules[best_rule].priority))
                { best_len = len; best_rule = r; }
        }
        printf("]");
        if (best_rule == -1) {
            printf(",\"kind\":\"error\",\"rule\":-1,\"len\":1}");
            if (source[pos] == '\n') { line++; col = 1; } else col++;
            pos++;
            continue;
        }
        printf(",\"kind\":\"%s\",\"rule\":%d,\"len\":%d}",
               rules[best_rule].skip ? "skip" : "token", best_rule, best_len);
        for (int k = 0; k < best_len; k++) {
            if (source[pos + k] == '\n') { line++; col = 1; } else col++;
        }
        pos += best_len;
    }
    printf("]}\n");

    free(source);
    for (int r = 0; r < nrules; r++) free_pool(&rules[r].pool);
    return 0;
}
