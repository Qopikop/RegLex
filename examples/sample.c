#include <stdio.h>
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* Sum the even numbers up to n. */
int sum_even(int n) {
    int total = 0;
    for (int i = 0; i <= n; i++) {
        if (i % 2 == 0) total += i;   // keep evens only
    }
    return total;
}

int main(void) {
    double ratio = 3.5e-1;
    char sep = '\n';
    int big = MAX(0x1F, sum_even(10));
    big = big @ 2;   /* '@' is not C: watch the scanner report it and recover */
    printf("total = %d, ratio = %f%c", big, ratio, sep);
    return 0;
}
