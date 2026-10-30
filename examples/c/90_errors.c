/* Error demo: lexical, syntax and semantic errors in one file.
 * The compiler reports each one with its location and keeps going. */
#include <stdio.h>

int add(int a, int b) {
    return a + b
}

int main(void) {
    int total = 0;
    int total = 5;
    count = total + 1;
    int price = 10 $ 2;
    total = add(1);
    printf("%d\n", total);
    return 0;
}
