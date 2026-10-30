# Optimization showcase for Python. Values are dynamically typed, so the
# optimizer only rewrites an operation when it can prove the operands are
# plain ints (e.g. a loop counter from range()); "x * 1" on an unknown x is
# kept, because x could be an object with its own __mul__.


def folding():
    seconds = 60 * 60 * 24
    weeks = seconds * 7
    return weeks // 3600


def propagation(n):
    scale = 4
    offset = scale * 2 + 1
    total = 0
    for i in range(n):
        total += i * scale + offset
    return total


def proven_ints(n):
    acc = 0
    for i in range(n):
        doubled = i * 2          # i is a proven int: strength-reduced to i + i
        same = doubled + 0       # and simplified
        acc += same
    return acc


def unknown_types(x):
    a = x * 1                    # kept: x might be a str, a list or an object
    return a


def dead_code(x):
    debug = False
    unused = x * 37
    if debug:
        print("debug", x)
    return x + 1


print("folding =", folding())
print("propagation =", propagation(10))
print("proven ints =", proven_ints(10))
print("unknown types =", unknown_types(21), unknown_types("ab"))
print("dead code =", dead_code(41))
