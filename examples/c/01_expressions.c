/* Basic: declarations, operator precedence and constant expressions.
 * Watch the optimizer fold the constant arithmetic and drop the unused value. */
#include <stdio.h>

#define WIDTH 12
#define HEIGHT 5

int main(void) {
    int a = 7, b = 3;
    int result = a + b * 2;            /* * binds tighter than + */
    int grouped = (a + b) * 2;
    int area = WIDTH * HEIGHT;         /* both operands are constants */
    int border = 2 * (WIDTH + HEIGHT) - 4;
    int unused = area * 100;           /* never read: dead code */
    int remainder = -17 % 5;           /* C: the sign follows the dividend */
    int quotient = -17 / 5;            /* C: division truncates toward zero */
    double ratio = (double)area / border;
    unsigned int mask = 0xF0u | 0x0Fu;
    long big = 1L << 40;

    printf("result=%d grouped=%d\n", result, grouped);
    printf("area=%d border=%d ratio=%.4f\n", area, border, ratio);
    printf("-17 / 5 = %d, -17 %% 5 = %d\n", quotient, remainder);
    printf("mask=%u big=%ld\n", mask, big);
    return 0;
}
