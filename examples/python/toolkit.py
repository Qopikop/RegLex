# Advanced: a multi-function toolkit -- deterministic random numbers,
# statistics, dynamic programming, text processing, bit tricks and an
# expression evaluator (shunting-yard). Everything here translates to TAC.
import math

SAMPLES = 40
seed = 12345


def next_random():
    global seed
    seed = (seed * 1103515245 + 12345) % 2147483648
    return (seed // 65536) % 32768


def clamp(x, lo, hi):
    return max(lo, min(hi, x))


# ---------- statistics ----------

def mean(values):
    return sum(values) / len(values)


def variance(values):
    m = mean(values)
    acc = 0.0
    for v in values:
        acc += (v - m) ** 2
    return acc / len(values)


def median(values):
    ordered = sorted(values)
    n = len(ordered)
    if n % 2 == 1:
        return ordered[n // 2]
    return (ordered[n // 2 - 1] + ordered[n // 2]) / 2


def histogram(values, buckets):
    counts = [0] * buckets
    for v in values:
        counts[clamp(v * buckets // 100, 0, buckets - 1)] += 1
    for b, count in enumerate(counts):
        print(f"  bucket {b} | {'#' * count} {count}")


# ---------- dynamic programming ----------

def edit_distance(x, y):
    n, m = len(x), len(y)
    d = []
    for i in range(n + 1):
        d.append([0] * (m + 1))
        d[i][0] = i
    for j in range(m + 1):
        d[0][j] = j
    for i in range(1, n + 1):
        for j in range(1, m + 1):
            cost = 0 if x[i - 1] == y[j - 1] else 1
            d[i][j] = min(d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost)
    return d[n][m]


def longest_increasing(values):
    best = [1] * len(values)
    for i in range(len(values)):
        for j in range(i):
            if values[j] < values[i] and best[j] + 1 > best[i]:
                best[i] = best[j] + 1
    return max(best)


def count_paths(rows, cols):
    grid = []
    for r in range(rows):
        grid.append([1] * cols)
        for c in range(cols):
            if r > 0 and c > 0:
                grid[r][c] = grid[r - 1][c] + grid[r][c - 1]
    return grid[rows - 1][cols - 1]


def memo_fib(n, cache=None):
    if cache is None:
        cache = [-1] * (n + 1)
    if n < 2:
        return n
    if cache[n] >= 0:
        return cache[n]
    cache[n] = memo_fib(n - 1, cache) + memo_fib(n - 2, cache)
    return cache[n]


# ---------- text ----------

def caesar(text, shift):
    out = []
    for ch in text:
        if "a" <= ch <= "z":
            out.append(chr(ord("a") + (ord(ch) - ord("a") + shift) % 26))
        elif "A" <= ch <= "Z":
            out.append(chr(ord("A") + (ord(ch) - ord("A") + shift) % 26))
        else:
            out.append(ch)
    return "".join(out)


def is_palindrome(s):
    cleaned = s.replace(" ", "").lower()
    return cleaned == cleaned[::-1]


def word_lengths(sentence):
    words = sentence.split()
    lengths = []
    for w in words:
        lengths.append(len(w.strip(".,")))
    return words, lengths


# ---------- bits ----------

def popcount(x):
    count = 0
    while x:
        x &= x - 1
        count += 1
    return count


def to_binary(x, width):
    digits = []
    for i in range(width - 1, -1, -1):
        digits.append("1" if (x >> i) & 1 else "0")
    return "".join(digits)


# ---------- expressions ----------

def precedence(op):
    if op in "*/%":
        return 2
    if op in "+-":
        return 1
    return 0


def apply(a, b, op):
    if op == "+":
        return a + b
    if op == "-":
        return a - b
    if op == "*":
        return a * b
    if op == "/":
        return a // b
    return a % b


def evaluate(expr):
    values, ops = [], []
    i = 0
    while i < len(expr):
        ch = expr[i]
        if ch == " ":
            i += 1
            continue
        if ch.isdigit():
            v = 0
            while i < len(expr) and expr[i].isdigit():
                v = v * 10 + int(expr[i])
                i += 1
            values.append(v)
            continue
        if ch == "(":
            ops.append(ch)
        elif ch == ")":
            while ops and ops[-1] != "(":
                b, a = values.pop(), values.pop()
                values.append(apply(a, b, ops.pop()))
            ops.pop()
        else:
            while ops and precedence(ops[-1]) >= precedence(ch):
                b, a = values.pop(), values.pop()
                values.append(apply(a, b, ops.pop()))
            ops.append(ch)
        i += 1
    while ops:
        b, a = values.pop(), values.pop()
        values.append(apply(a, b, ops.pop()))
    return values[0]


def main():
    sample = []
    for _ in range(SAMPLES):
        sample.append(next_random() % 100)
    print(f"== statistics over {SAMPLES} samples ==")
    print("min =", min(sample), "max =", max(sample), "median =", median(sample))
    print(f"mean = {mean(sample):.3f}  variance = {variance(sample):.3f}  stdev = {math.sqrt(variance(sample)):.3f}")
    histogram(sample, 5)

    print("== dynamic programming ==")
    print("edit_distance(kitten, sitting) =", edit_distance("kitten", "sitting"))
    print("longest increasing subsequence =", longest_increasing(sample))
    print("lattice paths 10x10 =", count_paths(10, 10), " fib(90) =", memo_fib(90))

    print("== text ==")
    secret = caesar("Regular Expressions", 3)
    print("caesar:", secret, "->", caesar(secret, 23))
    words, lengths = word_lengths("Finite automata recognise regular languages.")
    print("palindrome:", is_palindrome("Never odd or even"), " longest word:", words[lengths.index(max(lengths))])

    print("== bits ==")
    x = 0xF0F0A5
    print("popcount =", popcount(x), " binary =", to_binary(x, 24), " 2**40 > 0:", 2 ** 40 > 0)

    print("== expressions ==")
    for expr in ["2 + 3 * (7 - 4) % 5", "(10 + 20) * 3 - 4 / 2", "100 - 7 * (8 - 3)"]:
        print(f"{expr} = {evaluate(expr)}")


if __name__ == "__main__":
    main()
