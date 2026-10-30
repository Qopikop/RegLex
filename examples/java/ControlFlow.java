// Intermediate: static methods, nested loops, switch, do-while, for-each,
// break/continue and static fields.
public class ControlFlow {
    static int checks = 0;

    static boolean isPrime(int n) {
        checks++;
        if (n < 2) return false;
        if (n % 2 == 0) return n == 2;
        for (int d = 3; d * d <= n; d += 2) {
            if (n % d == 0) return false;
        }
        return true;
    }

    static int collatzLength(long n) {
        int steps = 0;
        while (n != 1) {
            n = (n % 2 == 0) ? n / 2 : 3 * n + 1;
            steps++;
        }
        return steps;
    }

    static String classify(int score) {
        if (score >= 85) return "distinction";
        else if (score >= 70) return "merit";
        else if (score >= 50) return "pass";
        return "fail";
    }

    static int digitSum(int n) {
        int s = 0;
        do {
            s += n % 10;
            n /= 10;
        } while (n > 0);
        return s;
    }

    public static void main(String[] args) {
        int primes = 0, longest = 0, longestAt = 0;
        String line = "";
        for (int i = 1; i <= 30; i++) {
            if (isPrime(i)) {
                line = line + i + " ";
                primes++;
            }
            int len = collatzLength(i);
            if (len > longest) { longest = len; longestAt = i; }
        }
        System.out.println(line);
        System.out.println(primes + " primes below 30 (" + checks + " checks)");
        System.out.println("longest Collatz chain: " + longest + " steps from " + longestAt);

        int[] scores = {92, 47, 70, 68, 85, 50};
        int passed = 0;
        for (int s : scores) {
            String g = classify(s);
            if (g.equals("fail")) continue;
            passed++;
            System.out.println("score " + s + " -> " + g);
        }
        System.out.println(passed + " of " + scores.length + " passed");

        String text = "the quick brown fox jumps over the lazy dog";
        int vowels = 0, es = 0, words = 1;
        for (int i = 0; i < text.length(); i++) {
            switch (text.charAt(i)) {
                case 'e': es++;
                case 'a': case 'i': case 'o': case 'u':
                    vowels++;
                    break;
                case ' ':
                    words++;
                    break;
                default:
                    break;
            }
        }
        System.out.println("vowels=" + vowels + " e=" + es + " words=" + words + " digitSum=" + digitSum(98765));
    }
}
