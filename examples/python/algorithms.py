# Intermediate: lists, recursion, merge sort, binary search, tuples and
# nested lists as matrices.


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def gcd(a, b):
    while b:
        a, b = b, a % b
    return a


def merge_sort(xs):
    if len(xs) <= 1:
        return xs
    mid = len(xs) // 2
    left = merge_sort(xs[:mid])
    right = merge_sort(xs[mid:])
    merged = []
    i = j = 0
    while i < len(left) and j < len(right):
        if left[i] <= right[j]:
            merged.append(left[i])
            i += 1
        else:
            merged.append(right[j])
            j += 1
    merged.extend(left[i:])
    merged.extend(right[j:])
    return merged


def binary_search(xs, key):
    lo, hi = 0, len(xs) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        if xs[mid] == key:
            return mid
        if xs[mid] < key:
            lo = mid + 1
        else:
            hi = mid - 1
    return -1


def matmul(a, b):
    n = len(a)
    c = []
    for i in range(n):
        row = []
        for j in range(n):
            total = 0
            for k in range(n):
                total += a[i][k] * b[k][j]
            row.append(total)
        c.append(row)
    return c


def min_max(xs):
    lo = hi = xs[0]
    for x in xs[1:]:
        if x < lo:
            lo = x
        if x > hi:
            hi = x
    return lo, hi


data = [42, 7, 19, 3, 88, 51, 23, 7, 64, 12, 5, 30]
ordered = merge_sort(data)
print("sorted:", ordered)
print("search 51 ->", binary_search(ordered, 51), " search 50 ->", binary_search(ordered, 50))
print("fib(20) =", fib(20))
print("gcd(462, 1071) =", gcd(462, 1071), " min/max:", min_max(data))
m = 4
a = []
b = []
for i in range(m):
    a.append([0] * m)
    b.append([0] * m)
    for j in range(m):
        a[i][j] = i + j
        b[i][j] = 2 if i == j else i - j
c = matmul(a, b)
for row in c:
    print(row)
trace = 0
for i in range(m):
    trace += c[i][i]
grand = 0
for row in c:
    grand += sum(row)
print("trace =", trace, " sum of all entries =", grand)
