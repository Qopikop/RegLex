/* Intermediate: functions, nested loops, if/else chains, switch with
 * fall-through, do-while, break/continue and global state. */
#include <stdio.h>

#define LIMIT 30

enum Grade { FAIL, PASS, MERIT, DISTINCTION };

int checks = 0;

int is_prime(int n) {
    checks++;
    if (n < 2) return 0;
    if (n % 2 == 0) return n == 2;
    for (int d = 3; d * d <= n; d += 2) {
        if (n % d == 0) return 0;
    }
    return 1;
}

int collatz_length(long n) {
    int steps = 0;
    while (n != 1) {
        n = (n % 2 == 0) ? n / 2 : 3 * n + 1;
        steps++;
    }
    return steps;
}

enum Grade classify(int score) {
    if (score >= 85) return DISTINCTION;
    else if (score >= 70) return MERIT;
    else if (score >= 50) return PASS;
    return FAIL;
}

const char *grade_name(enum Grade g) {
    switch (g) {
    case DISTINCTION: return "distinction";
    case MERIT:       return "merit";
    case PASS:        return "pass";
    default:          return "fail";
    }
}

int digit_sum(int n) {
    int s = 0;
    do {
        s += n % 10;
        n /= 10;
    } while (n > 0);
    return s;
}

int main(void) {
    int primes = 0, longest = 0, longest_at = 0;
    for (int i = 1; i <= LIMIT; i++) {
        if (is_prime(i)) {
            printf("%d ", i);
            primes++;
        }
        int len = collatz_length(i);
        if (len > longest) { longest = len; longest_at = i; }
    }
    printf("\n%d primes below %d (%d checks)\n", primes, LIMIT, checks);
    printf("longest Collatz chain: %d steps from %d\n", longest, longest_at);

    int scores[6] = {92, 47, 70, 68, 85, 50};
    int counts[4] = {0};
    for (int i = 0; i < 6; i++) {
        enum Grade g = classify(scores[i]);
        counts[g]++;
        printf("score %3d -> %s\n", scores[i], grade_name(g));
    }

    /* switch fall-through: count vowels and how many are 'e' */
    const char *text = "the quick brown fox jumps over the lazy dog";
    int vowels = 0, es = 0, spaces = 0;
    for (int i = 0; text[i] != 0; i++) {
        switch (text[i]) {
        case 'e': es++;
        case 'a': case 'i': case 'o': case 'u':
            vowels++;
            break;
        case ' ':
            spaces++;
            continue;
        default:
            break;
        }
    }
    printf("vowels=%d e=%d words=%d digit_sum(98765)=%d\n", vowels, es, spaces + 1, digit_sum(98765));
    printf("grades: fail=%d pass=%d merit=%d distinction=%d\n", counts[FAIL], counts[PASS], counts[MERIT], counts[DISTINCTION]);
    return 0;
}
