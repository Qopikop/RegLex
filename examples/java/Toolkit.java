// Advanced: a multi-method toolkit -- deterministic random numbers,
// statistics, dynamic programming, string processing, bit tricks and an
// expression evaluator built on array stacks. Fully translated to TAC.
import java.util.Arrays;

public class Toolkit {
    static final int SAMPLES = 40;
    static long seed = 12345L;

    static int nextRandom() {
        seed = (seed * 1103515245L + 12345L) % 2147483648L;
        return (int) (seed / 65536L) % 32768;
    }

    static int clamp(int x, int lo, int hi) {
        return Math.max(lo, Math.min(hi, x));
    }

    // ---------- statistics ----------

    static double mean(int[] v) {
        long total = 0;
        for (int x : v) total += x;
        return (double) total / v.length;
    }

    static double variance(int[] v) {
        double m = mean(v);
        double acc = 0.0;
        for (int x : v) {
            double d = x - m;
            acc += d * d;
        }
        return acc / v.length;
    }

    static int median(int[] v) {
        int[] copy = new int[v.length];
        for (int i = 0; i < v.length; i++) copy[i] = v[i];
        Arrays.sort(copy);
        int n = copy.length;
        return n % 2 == 1 ? copy[n / 2] : (copy[n / 2 - 1] + copy[n / 2]) / 2;
    }

    static String histogram(int[] v, int buckets) {
        int[] counts = new int[buckets];
        for (int x : v) counts[clamp(x * buckets / 100, 0, buckets - 1)]++;
        String out = "";
        for (int b = 0; b < buckets; b++) {
            String bar = "";
            for (int k = 0; k < counts[b]; k++) bar += "#";
            out += "  bucket " + b + " | " + bar + " " + counts[b] + "\n";
        }
        return out;
    }

    // ---------- dynamic programming ----------

    static int editDistance(String x, String y) {
        int n = x.length(), m = y.length();
        int[][] d = new int[n + 1][m + 1];
        for (int i = 0; i <= n; i++) d[i][0] = i;
        for (int j = 0; j <= m; j++) d[0][j] = j;
        for (int i = 1; i <= n; i++) {
            for (int j = 1; j <= m; j++) {
                int cost = x.charAt(i - 1) == y.charAt(j - 1) ? 0 : 1;
                d[i][j] = Math.min(Math.min(d[i - 1][j] + 1, d[i][j - 1] + 1), d[i - 1][j - 1] + cost);
            }
        }
        return d[n][m];
    }

    static int longestIncreasing(int[] v) {
        int[] best = new int[v.length];
        int answer = 0;
        for (int i = 0; i < v.length; i++) {
            best[i] = 1;
            for (int j = 0; j < i; j++)
                if (v[j] < v[i] && best[j] + 1 > best[i]) best[i] = best[j] + 1;
            answer = Math.max(answer, best[i]);
        }
        return answer;
    }

    static long countPaths(int rows, int cols) {
        long[][] grid = new long[rows][cols];
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++)
                grid[r][c] = (r == 0 || c == 0) ? 1 : grid[r - 1][c] + grid[r][c - 1];
        return grid[rows - 1][cols - 1];
    }

    // ---------- strings ----------

    static String caesar(String in, int shift) {
        String out = "";
        for (int i = 0; i < in.length(); i++) {
            char ch = in.charAt(i);
            if (ch >= 'a' && ch <= 'z') ch = (char) ('a' + (ch - 'a' + shift) % 26);
            else if (ch >= 'A' && ch <= 'Z') ch = (char) ('A' + (ch - 'A' + shift) % 26);
            out += ch;
        }
        return out;
    }

    static boolean isPalindrome(String s) {
        int i = 0, j = s.length() - 1;
        while (i < j) {
            while (i < j && s.charAt(i) == ' ') i++;
            while (i < j && s.charAt(j) == ' ') j--;
            if (s.charAt(i) != s.charAt(j)) return false;
            i++;
            j--;
        }
        return true;
    }

    static String reverseWords(String s) {
        String result = "", word = "";
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            if (ch == ' ') {
                if (!word.isEmpty()) result = word + (result.isEmpty() ? "" : " ") + result;
                word = "";
            } else word += ch;
        }
        if (!word.isEmpty()) result = word + (result.isEmpty() ? "" : " ") + result;
        return result;
    }

    // ---------- bits ----------

    static int popcount(int x) {
        int c = 0;
        while (x != 0) { x &= x - 1; c++; }
        return c;
    }

    static int reverseBits(int x) {
        int r = 0;
        for (int i = 0; i < 32; i++) {
            r = (r << 1) | (x & 1);
            x >>>= 1;
        }
        return r;
    }

    // ---------- expressions ----------

    static int precedence(char op) {
        if (op == '*' || op == '/' || op == '%') return 2;
        if (op == '+' || op == '-') return 1;
        return 0;
    }

    static int apply(int a, int b, char op) {
        switch (op) {
            case '+': return a + b;
            case '-': return a - b;
            case '*': return a * b;
            case '/': return b != 0 ? a / b : 0;
            default:  return b != 0 ? a % b : 0;
        }
    }

    static int evaluate(String expr) {
        int[] values = new int[32];
        char[] ops = new char[32];
        int nv = 0, no = 0;
        for (int i = 0; i < expr.length(); i++) {
            char ch = expr.charAt(i);
            if (ch == ' ') continue;
            if (ch >= '0' && ch <= '9') {
                int v = 0;
                while (i < expr.length() && expr.charAt(i) >= '0' && expr.charAt(i) <= '9') { v = v * 10 + (expr.charAt(i) - '0'); i++; }
                i--;
                values[nv++] = v;
            } else if (ch == '(') {
                ops[no++] = ch;
            } else if (ch == ')') {
                while (no > 0 && ops[no - 1] != '(') { int b = values[--nv]; int a = values[--nv]; values[nv++] = apply(a, b, ops[--no]); }
                no--;
            } else {
                while (no > 0 && precedence(ops[no - 1]) >= precedence(ch)) { int b = values[--nv]; int a = values[--nv]; values[nv++] = apply(a, b, ops[--no]); }
                ops[no++] = ch;
            }
        }
        while (no > 0) { int b = values[--nv]; int a = values[--nv]; values[nv++] = apply(a, b, ops[--no]); }
        return values[0];
    }

    public static void main(String[] args) {
        int[] sample = new int[SAMPLES];
        for (int i = 0; i < SAMPLES; i++) sample[i] = nextRandom() % 100;
        int lo = sample[0], hi = sample[0];
        for (int x : sample) { lo = Math.min(lo, x); hi = Math.max(hi, x); }
        System.out.println("== statistics over " + SAMPLES + " samples ==");
        System.out.println("min=" + lo + " max=" + hi + " median=" + median(sample));
        System.out.printf("mean=%.3f variance=%.3f%n", mean(sample), variance(sample));
        System.out.print(histogram(sample, 5));

        System.out.println("== dynamic programming ==");
        System.out.println("editDistance(kitten, sitting) = " + editDistance("kitten", "sitting"));
        System.out.println("longest increasing subsequence = " + longestIncreasing(sample));
        System.out.println("lattice paths 10x10 = " + countPaths(10, 10));

        System.out.println("== strings ==");
        String secret = caesar("Regular Expressions", 3);
        System.out.println("caesar: " + secret + " -> " + caesar(secret, 23));
        System.out.println("palindrome: " + isPalindrome("never odd or even") + ", reversed: " + reverseWords("finite automata are fun"));

        System.out.println("== bits ==");
        int x = 0xF0F0A5;
        System.out.println("popcount=" + popcount(x) + " reversed=" + reverseBits(x) + " negative popcount=" + popcount(-1));

        System.out.println("== expressions ==");
        System.out.println("2 + 3 * (7 - 4) % 5 = " + evaluate("2 + 3 * (7 - 4) % 5"));
        System.out.println("(10 + 20) * 3 - 4 / 2 = " + evaluate("(10 + 20) * 3 - 4 / 2"));
    }
}
