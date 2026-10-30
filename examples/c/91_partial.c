/* Partial support demo: structs, pointers and malloc are lexed and parsed,
 * but the TAC translator keeps them as opaque operations (shown and counted,
 * never silently dropped), and the optimizer treats them conservatively. */
#include <stdio.h>
#include <stdlib.h>

struct Point {
    int x;
    int y;
};

int manhattan(struct Point *p) {
    return p->x + p->y;
}

int main(void) {
    struct Point p;
    p.x = 3;
    p.y = 4;
    int *buffer = malloc(10 * sizeof(int));
    int n = 2 + 3;                         /* still folded */
    int value = manhattan(&p) + n;
    printf("%d\n", value);
    free(buffer);
    return 0;
}
