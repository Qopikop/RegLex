// Optimization showcase for Java. Note the language-specific choices:
// x * 8 becomes x << 3 (exact under Java's wrapping arithmetic), while
// division by a variable is kept because it may throw ArithmeticException.
public class Optimizations {
    static int folding() {
        int seconds = 60 * 60 * 24;
        int weeks = seconds * 7;
        return weeks / 3600;
    }

    static int propagation(int n) {
        int scale = 4;
        int offset = scale * 2 + 1;
        int total = 0;
        for (int i = 0; i < n; i++) total += i * scale + offset;
        return total;
    }

    static int commonSubexpressions(int a, int b) {
        int x = (a + b) * (a + b);
        int y = (a + b) * 3;
        int z = a * b + a * b;
        return x + y + z;
    }

    static int loopInvariant(int[] v, int k) {
        int count = 0;
        for (int i = 0; i < v.length; i++) {
            int limit = k * k + 1;
            if (v[i] < limit) count++;
        }
        return count;
    }

    static int strength(int x) {
        int a = x * 8;
        int b = a + 0;
        int c = b * 1;
        return c - (x - x);
    }

    static int keepsDivision(int x, int y) {
        int unusedQuotient = x / y;     // may throw: not removed even though unused
        return x + 1;
    }

    public static void main(String[] args) {
        int[] data = {3, 9, 1, 12, 5, 7, 2, 10};
        System.out.println("folding=" + folding());
        System.out.println("propagation=" + propagation(10));
        System.out.println("cse=" + commonSubexpressions(3, 4));
        System.out.println("licm=" + loopInvariant(data, 2));
        System.out.println("strength=" + strength(21));
        System.out.println("division=" + keepsDivision(41, 2));
    }
}
