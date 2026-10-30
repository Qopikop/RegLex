# Basic: dynamic values, operator precedence, Python's floor division and
# modulo, true division, unbounded-looking integers and string operations.
WIDTH = 12
HEIGHT = 5

a, b = 7, 3
result = a + b * 2
grouped = (a + b) * 2
area = WIDTH * HEIGHT
unused = area * 100
ratio = area / (2 * (WIDTH + HEIGHT))
print("result =", result, "grouped =", grouped)
print("area =", area, "ratio =", ratio)
print("-17 // 5 =", -17 // 5, " -17 % 5 =", -17 % 5)     # floor semantics
print("2 ** 62 =", 2 ** 62, " 7 / 2 =", 7 / 2)
name = "compiler"
print(name.upper(), len(name), name[0] + name[-1], name * 2)
print(f"{area:>6} | {ratio:.3f} | {name!r}")
print(1 < 2 < 3, 3 > 2 > 2, "pi" in "pipeline", None is None)
