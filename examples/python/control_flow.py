# Intermediate: functions with default arguments, while/for loops, elif
# chains, for-else, break/continue, enumerate and global state.
checks = 0


def is_prime(n):
    global checks
    checks += 1
    if n < 2:
        return False
    if n % 2 == 0:
        return n == 2
    d = 3
    while d * d <= n:
        if n % d == 0:
            return False
        d += 2
    return True


def collatz_length(n):
    steps = 0
    while n != 1:
        n = n // 2 if n % 2 == 0 else 3 * n + 1
        steps += 1
    return steps


def classify(score, pass_mark=50):
    if score >= 85:
        return "distinction"
    elif score >= 70:
        return "merit"
    elif score >= pass_mark:
        return "pass"
    return "fail"


def first_multiple(values, k):
    for i, v in enumerate(values):
        if v % k == 0:
            print("first multiple of", k, "is", v, "at index", i)
            break
    else:
        print("no multiple of", k)


primes = []
longest, longest_at = 0, 0
for i in range(1, 31):
    if is_prime(i):
        primes.append(i)
    length = collatz_length(i)
    if length > longest:
        longest, longest_at = length, i
print(primes)
print(len(primes), "primes below 30 (", checks, "checks )")
print("longest Collatz chain:", longest, "steps from", longest_at)

scores = [92, 47, 70, 68, 85, 50]
passed = 0
for s in scores:
    grade = classify(s)
    if grade == "fail":
        continue
    passed += 1
    print(f"score {s:3d} -> {grade}")
print(passed, "of", len(scores), "passed; strict:", classify(55, pass_mark=60))

first_multiple(scores, 17)
first_multiple(scores, 7)

text = "the quick brown fox jumps over the lazy dog"
vowels = 0
for ch in text:
    if ch in "aeiou":
        vowels += 1
print("vowels =", vowels, "words =", len(text.split()))
