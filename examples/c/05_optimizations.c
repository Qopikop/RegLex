/* Optimization showcase: each function exercises particular passes.
 * Compare the original and optimized TAC and read the explanations. */
#include <stdio.h>

int folding(void) {
    int seconds = 60 * 60 * 24;          /* constant folding */
    int weeks = seconds * 7;
    return weeks / 3600;
}

int propagation(int n) {
    int scale = 4;                       /* constant propagation through the loop */
    int offset = scale * 2 + 1;
    int total = 0;
    for (int i = 0; i < n; i++) total += i * scale + offset;
    return total;
}

int common_subexpressions(int a, int b) {
    int x = (a + b) * (a + b);           /* a + b computed once (local value numbering) */
    int y = (a + b) * 3;
    int z = a * b + a * b;
    return x + y + z;
}

int loop_invariant(int v[], int n, int k) {
    int count = 0;
    for (int i = 0; i < n; i++) {
        int limit = k * k + 1;           /* does not change inside the loop: hoisted */
        if (v[i] < limit) count++;
    }
    return count;
}

int strength_and_algebra(int x) {
    int a = x * 2;                       /* strength reduction: x + x */
    int b = a + 0;                       /* algebraic simplification */
    int c = b * 1;
    int d = c - c;                       /* always 0 */
    return c + d;
}

int dead_and_unreachable(int x) {
    int debug = 0;
    int unused = x * 37;                 /* never read */
    if (debug) {                         /* debug is always 0: branch removed */
        printf("debug: %d\n", x);
    }
    return x + 1;
}

int unsafe_cases(int x, int y) {
    /* NOT optimized: dividing by a variable could trap, and x / x is not
       always 1 (x may be 0); keeping them preserves the program's behaviour */
    int q = x / y;
    int r = x / x;
    return q + r;
}

int main(void) {
    int data[8] = {3, 9, 1, 12, 5, 7, 2, 10};
    printf("folding=%d\n", folding());
    printf("propagation=%d\n", propagation(10));
    printf("cse=%d\n", common_subexpressions(3, 4));
    printf("licm=%d\n", loop_invariant(data, 8, 2));
    printf("strength=%d\n", strength_and_algebra(21));
    printf("dead=%d\n", dead_and_unreachable(41));
    printf("unsafe=%d\n", unsafe_cases(84, 2));
    return 0;
}
