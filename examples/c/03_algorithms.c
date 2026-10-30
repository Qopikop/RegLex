/* Intermediate: arrays, recursion, sorting, searching and 2-D matrices. */
#include <stdio.h>

#define N 12
#define M 4

int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int gcd(int a, int b) {
    return b == 0 ? a : gcd(b, a % b);
}

long power(long base, int exp) {
    long result = 1;
    while (exp > 0) {
        if (exp & 1) result *= base;
        base *= base;
        exp >>= 1;
    }
    return result;
}

void swap(int a[], int i, int j) {
    int t = a[i];
    a[i] = a[j];
    a[j] = t;
}

int partition(int a[], int lo, int hi) {
    int pivot = a[hi];
    int i = lo - 1;
    for (int j = lo; j < hi; j++) {
        if (a[j] <= pivot) {
            i++;
            swap(a, i, j);
        }
    }
    swap(a, i + 1, hi);
    return i + 1;
}

void quicksort(int a[], int lo, int hi) {
    if (lo < hi) {
        int p = partition(a, lo, hi);
        quicksort(a, lo, p - 1);
        quicksort(a, p + 1, hi);
    }
}

int binary_search(int a[], int n, int key) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid] == key) return mid;
        if (a[mid] < key) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

void print_array(const char *label, int a[], int n) {
    printf("%s:", label);
    for (int i = 0; i < n; i++) printf(" %d", a[i]);
    printf("\n");
}

int main(void) {
    int data[N] = {42, 7, 19, 3, 88, 51, 23, 7, 64, 12, 5, 30};
    print_array("input ", data, N);
    quicksort(data, 0, N - 1);
    print_array("sorted", data, N);
    printf("search 51 -> index %d, search 50 -> index %d\n", binary_search(data, N, 51), binary_search(data, N, 50));

    printf("fib:");
    for (int i = 0; i <= 15; i++) printf(" %d", fib(i));
    printf("\ngcd(462, 1071) = %d, 3^20 = %ld\n", gcd(462, 1071), power(3, 20));

    int a[M][M], b[M][M], c[M][M];
    for (int i = 0; i < M; i++)
        for (int j = 0; j < M; j++) {
            a[i][j] = i + j;
            b[i][j] = (i == j) ? 2 : i - j;
        }
    int trace = 0;
    for (int i = 0; i < M; i++) {
        for (int j = 0; j < M; j++) {
            int sum = 0;
            for (int k = 0; k < M; k++) sum += a[i][k] * b[k][j];
            c[i][j] = sum;
        }
        trace += c[i][i];
    }
    for (int i = 0; i < M; i++) {
        for (int j = 0; j < M; j++) printf("%5d", c[i][j]);
        printf("\n");
    }
    printf("trace = %d\n", trace);
    return 0;
}
