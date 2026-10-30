/* Advanced: a multi-function toolkit -- a deterministic random generator,
 * statistics, dynamic programming, text processing, bit manipulation and a
 * small expression evaluator (shunting-yard) written with plain arrays.
 * Every construct here is translated to TAC and executed. */
#include <stdio.h>
#include <string.h>

#define SAMPLES 40
#define MAXLEN 64
#define STACK 32

unsigned int seed = 12345u;

/* linear congruential generator: unsigned arithmetic wraps by definition */
unsigned int next_random(void) {
    seed = seed * 1103515245u + 12345u;
    return (seed / 65536u) % 32768u;
}

int clamp(int x, int lo, int hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

/* ---------- statistics ---------- */

double mean(int v[], int n) {
    long total = 0;
    for (int i = 0; i < n; i++) total += v[i];
    return (double)total / n;
}

double variance(int v[], int n) {
    double m = mean(v, n);
    double acc = 0.0;
    for (int i = 0; i < n; i++) {
        double d = v[i] - m;
        acc += d * d;
    }
    return acc / n;
}

int isqrt(int x) {                      /* integer square root by Newton's method */
    if (x < 2) return x;
    int r = x / 2;
    int prev = 0;
    while (r != prev && r != prev + 1) {
        prev = r;
        r = (r + x / r) / 2;
    }
    while (r * r > x) r--;
    return r;
}

void histogram(int v[], int n, int buckets, int width) {
    int counts[10] = {0};
    for (int i = 0; i < n; i++) {
        int b = v[i] * buckets / 100;
        counts[clamp(b, 0, buckets - 1)]++;
    }
    for (int b = 0; b < buckets; b++) {
        printf("  %2d-%2d | ", b * width, (b + 1) * width - 1);
        for (int k = 0; k < counts[b]; k++) printf("#");
        printf(" %d\n", counts[b]);
    }
}

/* ---------- dynamic programming ---------- */

int lcs(const char *x, const char *y) {
    int n = strlen(x), m = strlen(y);
    int table[MAXLEN][MAXLEN];
    for (int i = 0; i <= n; i++) {
        for (int j = 0; j <= m; j++) {
            if (i == 0 || j == 0) table[i][j] = 0;
            else if (x[i - 1] == y[j - 1]) table[i][j] = table[i - 1][j - 1] + 1;
            else table[i][j] = table[i - 1][j] > table[i][j - 1] ? table[i - 1][j] : table[i][j - 1];
        }
    }
    return table[n][m];
}

int knapsack(int weight[], int value[], int items, int capacity) {
    int best[64] = {0};
    for (int i = 0; i < items; i++) {
        for (int c = capacity; c >= weight[i]; c--) {
            int with = best[c - weight[i]] + value[i];
            if (with > best[c]) best[c] = with;
        }
    }
    return best[capacity];
}

long count_paths(int rows, int cols) {   /* lattice paths, Pascal-style DP */
    long grid[16][16];
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            grid[r][c] = (r == 0 || c == 0) ? 1 : grid[r - 1][c] + grid[r][c - 1];
    return grid[rows - 1][cols - 1];
}

/* ---------- text ---------- */

void caesar(const char *in, char out[], int shift) {
    int i = 0;
    for (; in[i] != 0; i++) {
        char ch = in[i];
        if (ch >= 'a' && ch <= 'z') ch = (char)('a' + (ch - 'a' + shift) % 26);
        else if (ch >= 'A' && ch <= 'Z') ch = (char)('A' + (ch - 'A' + shift) % 26);
        out[i] = ch;
    }
    out[i] = 0;
}

int is_palindrome(const char *s) {
    int i = 0, j = strlen(s) - 1;
    while (i < j) {
        while (i < j && s[i] == ' ') i++;
        while (i < j && s[j] == ' ') j--;
        if (s[i] != s[j]) return 0;
        i++;
        j--;
    }
    return 1;
}

int count_words(const char *s) {
    int words = 0, inside = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == ' ' || s[i] == '\t') inside = 0;
        else if (!inside) { inside = 1; words++; }
    }
    return words;
}

/* ---------- bits ---------- */

int popcount(unsigned int x) {
    int c = 0;
    while (x) { x &= x - 1; c++; }
    return c;
}

unsigned int reverse_bits(unsigned int x) {
    unsigned int r = 0;
    for (int i = 0; i < 32; i++) {
        r = (r << 1) | (x & 1u);
        x >>= 1;
    }
    return r;
}

/* ---------- expression evaluation (shunting-yard with array stacks) ---------- */

int precedence(char op) {
    if (op == '*' || op == '/' || op == '%') return 2;
    if (op == '+' || op == '-') return 1;
    return 0;
}

int apply(int a, int b, char op) {
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return b != 0 ? a / b : 0;
    case '%': return b != 0 ? a % b : 0;
    }
    return 0;
}

int evaluate(const char *expr) {
    int values[STACK], nv = 0;
    char ops[STACK];
    int no = 0;
    for (int i = 0; expr[i]; i++) {
        char ch = expr[i];
        if (ch == ' ') continue;
        if (ch >= '0' && ch <= '9') {
            int v = 0;
            while (expr[i] >= '0' && expr[i] <= '9') { v = v * 10 + (expr[i] - '0'); i++; }
            i--;
            values[nv++] = v;
        } else if (ch == '(') {
            ops[no++] = ch;
        } else if (ch == ')') {
            while (no > 0 && ops[no - 1] != '(') { int b = values[--nv], a = values[--nv]; values[nv++] = apply(a, b, ops[--no]); }
            no--;
        } else {
            while (no > 0 && precedence(ops[no - 1]) >= precedence(ch)) { int b = values[--nv], a = values[--nv]; values[nv++] = apply(a, b, ops[--no]); }
            ops[no++] = ch;
        }
    }
    while (no > 0) { int b = values[--nv], a = values[--nv]; values[nv++] = apply(a, b, ops[--no]); }
    return values[0];
}

int main(void) {
    int sample[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) sample[i] = next_random() % 100;
    int lo = sample[0], hi = sample[0];
    for (int i = 1; i < SAMPLES; i++) {
        if (sample[i] < lo) lo = sample[i];
        if (sample[i] > hi) hi = sample[i];
    }
    printf("== statistics over %d samples ==\n", SAMPLES);
    printf("min=%d max=%d mean=%.3f variance=%.3f isqrt(variance)=%d\n", lo, hi, mean(sample, SAMPLES),
           variance(sample, SAMPLES), isqrt((int)variance(sample, SAMPLES)));
    histogram(sample, SAMPLES, 5, 20);

    printf("== dynamic programming ==\n");
    printf("lcs(\"compiler design\", \"computer science\") = %d\n", lcs("compiler design", "computer science"));
    int weight[5] = {1, 3, 4, 5, 9};
    int value[5] = {1, 4, 5, 7, 11};
    printf("knapsack(capacity 12) = %d\n", knapsack(weight, value, 5, 12));
    printf("lattice paths 8x8 = %ld\n", count_paths(8, 8));

    printf("== text ==\n");
    char secret[MAXLEN];
    caesar("Regular Expressions", secret, 3);
    printf("caesar: %s\n", secret);
    char plain[MAXLEN];
    caesar(secret, plain, 23);
    printf("decoded: %s\n", plain);
    printf("palindrome('never odd or even') = %d, words = %d\n", is_palindrome("never odd or even"), count_words("  never odd  or even "));

    printf("== bits ==\n");
    unsigned int x = 0xF0F0A5u;
    printf("popcount(0x%x) = %d, reversed = 0x%08x\n", x, popcount(x), reverse_bits(x));

    printf("== expressions ==\n");
    printf("2 + 3 * (7 - 4) %% 5 = %d\n", evaluate("2 + 3 * (7 - 4) % 5"));
    printf("(10 + 20) * 3 - 4 / 2 = %d\n", evaluate("(10 + 20) * 3 - 4 / 2"));
    return 0;
}
