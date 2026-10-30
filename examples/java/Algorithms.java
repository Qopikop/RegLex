// Intermediate: arrays, recursion, merge sort, binary search and matrices.
import java.util.Arrays;

public class Algorithms {
    static int fib(int n) {
        if (n < 2) return n;
        return fib(n - 1) + fib(n - 2);
    }

    static int gcd(int a, int b) {
        return b == 0 ? a : gcd(b, a % b);
    }

    static long power(long base, int exp) {
        long result = 1;
        while (exp > 0) {
            if ((exp & 1) == 1) result *= base;
            base *= base;
            exp >>= 1;
        }
        return result;
    }

    static void mergeSort(int[] a, int[] tmp, int lo, int hi) {
        if (hi - lo < 2) return;
        int mid = (lo + hi) >>> 1;
        mergeSort(a, tmp, lo, mid);
        mergeSort(a, tmp, mid, hi);
        int i = lo, j = mid, k = lo;
        while (i < mid && j < hi) tmp[k++] = a[i] <= a[j] ? a[i++] : a[j++];
        while (i < mid) tmp[k++] = a[i++];
        while (j < hi) tmp[k++] = a[j++];
        for (k = lo; k < hi; k++) a[k] = tmp[k];
    }

    static int binarySearch(int[] a, int key) {
        int lo = 0, hi = a.length - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            if (a[mid] == key) return mid;
            if (a[mid] < key) lo = mid + 1;
            else hi = mid - 1;
        }
        return -1;
    }

    static int[][] multiply(int[][] a, int[][] b) {
        int n = a.length;
        int[][] c = new int[n][n];
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) {
                int sum = 0;
                for (int k = 0; k < n; k++) sum += a[i][k] * b[k][j];
                c[i][j] = sum;
            }
        return c;
    }

    public static void main(String[] args) {
        int[] data = {42, 7, 19, 3, 88, 51, 23, 7, 64, 12, 5, 30};
        System.out.println("input  " + Arrays.toString(data));
        mergeSort(data, new int[data.length], 0, data.length);
        System.out.println("sorted " + Arrays.toString(data));
        System.out.println("search 51 -> " + binarySearch(data, 51) + ", search 50 -> " + binarySearch(data, 50));

        String fibs = "fib:";
        for (int i = 0; i <= 15; i++) fibs += " " + fib(i);
        System.out.println(fibs);
        System.out.println("gcd(462, 1071) = " + gcd(462, 1071) + ", 3^20 = " + power(3, 20));

        int m = 4;
        int[][] a = new int[m][m];
        int[][] b = new int[m][m];
        for (int i = 0; i < m; i++)
            for (int j = 0; j < m; j++) {
                a[i][j] = i + j;
                b[i][j] = (i == j) ? 2 : i - j;
            }
        int[][] c = multiply(a, b);
        int trace = 0;
        for (int i = 0; i < m; i++) {
            System.out.println(Arrays.toString(c[i]));
            trace += c[i][i];
        }
        System.out.println("trace = " + trace);
    }
}
