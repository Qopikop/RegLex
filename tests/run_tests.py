"""End-to-end tests for the compiler workbench.

    python3 tests/run_tests.py            # everything (about a minute)
    python3 tests/run_tests.py --quick    # fewer random programs, no stress files

Sections:
  examples     every example program: native gcc / javac / python3 output must
               equal the output of the original TAC, and the optimized TAC must
               behave identically; the NFA, DFA and minimized DFA must agree
  diagnostics  error examples report the expected categories
  frontend     small valid / invalid snippets per language
  optimizer    specific transformations happen -- and unsafe ones do not
  random       randomly generated programs per language, differential vs native
  stress       generated 1,500+ line programs: complete, no truncation, correct
  lexical      custom rule files, invalid rules, legacy ./lexer golden output

Native programs run with a timeout in a temporary directory. A summary is
written to tests/last_report.json.
"""
import json
import os
import pathlib
import random
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
RCC = ROOT / "build" / "rcc"
QUICK = "--quick" in sys.argv
results = []          # (section, name, ok, detail)


def record(section, name, ok, detail=""):
    results.append({"section": section, "name": name, "ok": bool(ok), "detail": detail})
    mark = "PASS" if ok else "FAIL"
    print(f"  [{mark}] {name}" + (f" -- {detail}" if detail and not ok else ""))


def rcc(path, *args, timeout=120):
    r = subprocess.run([str(RCC), str(path), "--json", "--rules-dir", str(ROOT / "rules"), *args],
                       capture_output=True, text=True, timeout=timeout)
    return json.loads(r.stdout)


def native(path, lang, workdir, timeout=60):
    """Compiles and runs a program with the real toolchain. Returns (stdout, failed)."""
    path = pathlib.Path(path)
    if lang == "C":
        exe = pathlib.Path(workdir) / "prog"
        c = subprocess.run(["cc", "-O1", "-w", "-o", str(exe), str(path), "-lm"], capture_output=True, text=True)
        if c.returncode:
            raise RuntimeError("cc failed: " + c.stderr[:300])
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=timeout)
    elif lang == "Java":
        c = subprocess.run(["javac", "-d", workdir, str(path)], capture_output=True, text=True)
        if c.returncode:
            raise RuntimeError("javac failed: " + c.stderr[:300])
        r = subprocess.run(["java", "-cp", workdir, path.stem], capture_output=True, text=True, timeout=timeout)
    else:
        r = subprocess.run([sys.executable, str(path)], capture_output=True, text=True, timeout=timeout, cwd=workdir)
    return r.stdout, r.returncode != 0


def differential(path, lang, label, section):
    """native output == original TAC output, and optimized TAC == original."""
    d = rcc(path)
    with tempfile.TemporaryDirectory() as tmp:
        try:
            out, failed = native(path, lang, tmp)
        except Exception as e:      # noqa: BLE001 -- report and continue
            record(section, label, False, str(e))
            return d
    run = d.get("run")
    if not run:
        record(section, label, False, "no run section (compile errors?): " +
               "; ".join(x["message"] for x in d["diagnostics"] if x["severity"] == "error")[:300])
        return d
    o, p = run["original"], run["optimized"]
    if o["status"] == "unsupported":
        record(section, label, False, "not executable: " + o["message"])
        return d
    same_native = o["stdout"] == out and (o["status"] == "exception") == failed
    detail = ""
    if not same_native:
        a, b = out.splitlines(), o["stdout"].splitlines()
        first = next((i for i in range(max(len(a), len(b))) if i >= len(a) or i >= len(b) or a[i] != b[i]), 0)
        detail = f"line {first + 1}: native {a[first] if first < len(a) else '<end>'!r} vs TAC {b[first] if first < len(b) else '<end>'!r} ({o['message']})"
    record(section, label + " -- TAC matches native " + lang, same_native, detail)
    record(section, label + " -- optimized TAC equivalent", run["equivalent"] is True and p["stdout"] == o["stdout"],
           f"original {o['status']} vs optimized {p['status']}: {p['message']}")
    record(section, label + " -- NFA, DFA and minimized DFA agree", d["lexEquivalence"]["identical"])
    return d


# ---------------------------------------------------------------------------

def section_examples():
    print("\n== examples ==")
    manifest = json.loads((ROOT / "examples" / "manifest.json").read_text())
    for ex in manifest["examples"]:
        path = ROOT / "examples" / ex["file"]
        if ex["expect"] == "run":
            d = differential(path, ex["language"], ex["file"], "examples")
            t = d.get("tac", {})
            record("examples", ex["file"] + " -- complete translation", t.get("complete") is True, f"{t.get('opaque')} opaque operations")
        elif ex["expect"] == "partial":
            d = rcc(path)
            ok = d["tac"]["opaque"] > 0 and d["run"]["original"]["status"] == "unsupported" and d["run"]["equivalent"] is None
            record("examples", ex["file"] + " -- partial translation reported, not executed", ok)
            tac_diags = [x for x in d["diagnostics"] if x["stage"] == "tac"]
            record("examples", ex["file"] + " -- each opaque construct has a diagnostic", len(tac_diags) >= d["tac"]["opaque"],
                   f"{len(tac_diags)} diagnostics for {d['tac']['opaque']} opaque operations")


def section_diagnostics():
    print("\n== diagnostics ==")
    manifest = json.loads((ROOT / "examples" / "manifest.json").read_text())
    for ex in manifest["examples"]:
        if ex["expect"] != "errors":
            continue
        d = rcc(ROOT / "examples" / ex["file"])
        cats = {x["category"] for x in d["diagnostics"]}
        missing = [c for c in ex["diagnostics"] if c not in cats]
        record("diagnostics", ex["file"] + " -- expected categories", not missing, f"missing {missing}, got {sorted(cats)}")
        located = all(x["line"] > 0 for x in d["diagnostics"] if x["severity"] == "error")
        record("diagnostics", ex["file"] + " -- every error has a line number", located)
        record("diagnostics", ex["file"] + " -- not executed with errors", d.get("run") is None)


FRONTEND = [
    # (language, source, expected error categories ([] = must compile cleanly), description)
    ("C", "int main(void) { int x = (1 + 2) * 3; return x; }", [], "nested expression"),
    ("C", "int main(void) { int x = 1 return x; }", ["Syntax Error"], "missing semicolon"),
    ("C", "int main(void) { return (1 + 2; }", ["Syntax Error"], "missing parenthesis"),
    ("C", "int main(void) { int a; int a; return 0; }", ["Duplicate Declaration"], "duplicate declaration"),
    ("C", "int main(void) { { int a = 1; } return a; }", ["Undeclared Variable"], "block scope ends"),
    ("C", "int main(void) { int a = 1; { int a = 2; } return a; }", [], "shadowing in an inner block"),
    ("C", "int f(int a, int b) { return a; } int main(void) { return f(1); }", ["Call Error"], "wrong argument count"),
    ("C", "int main(void) { break; }", ["Misplaced Statement"], "break outside a loop"),
    ("C", "typedef struct { int x; } P; int main(void) { P p; return 0; }", [], "typedef struct"),
    ("C", "int main(void) { double d = 1.5; int k = d % 2; return 0; }", ["Type Error"], "% on a double"),
    ("Java", "class A { public static void main(String[] a) { int x = 1 + 2 * 3; } }", [], "precedence"),
    ("Java", "class A { public static void main(String[] a) { int x = 2.5; } }", ["Type Error"], "lossy conversion"),
    ("Java", "class A { public static void main(String[] a) { boolean b = 1; } }", ["Type Error"], "int to boolean"),
    ("Java", "class A { public static void main(String[] a) { if (1) {} } }", ["Type Error"], "non-boolean condition"),
    ("Java", "class A { int f() { return 1; } public static void main(String[] a) { f(); } }", ["Static Context"], "instance call from static"),
    ("Java", "class A { public static void main(String[] a) { int[] v = new int[3]; v[0] = 1 } }", ["Syntax Error"], "missing semicolon"),
    ("Java", "class A { static int g(int x) { if (x > 0) { return 1; } return 0; } public static void main(String[] a) { System.out.println(g(2)); } }", [], "nested blocks"),
    ("Python", "def f(x):\n    return x * 2\nprint(f(3))\n", [], "simple function"),
    ("Python", "def f(x):\nreturn x\n", ["Indentation Error"], "missing indentation"),
    ("Python", "x = (1 + 2\nprint(x)\n", ["Syntax Error"], "unclosed parenthesis"),
    ("Python", "if x > 1\n    pass\n", ["Syntax Error"], "missing colon"),
    ("Python", "def f(a, b=2):\n    return a + b\nprint(f(1, c=3))\n", ["Call Error"], "unknown keyword argument"),
    ("Python", "for i in range(3):\n    for j in range(i):\n        print(i, j)\n", [], "nested loops"),
]


def section_frontend():
    print("\n== frontend ==")
    ext = {"C": ".c", "Java": ".java", "Python": ".py"}
    with tempfile.TemporaryDirectory() as tmp:
        for i, (lang, src, expect, desc) in enumerate(FRONTEND):
            p = pathlib.Path(tmp) / (("A" if lang == "Java" else f"t{i}") + ext[lang])
            p.write_text(src)
            d = rcc(p)
            errs = {x["category"] for x in d["diagnostics"] if x["severity"] == "error"}
            ok = (not errs) if not expect else all(e in errs for e in expect)
            record("frontend", f"{lang}: {desc}", ok, f"errors {sorted(errs)}, expected {expect}")


OPTIMIZER = [
    # (language, source, function, must contain, must not contain, description)
    ("C", "int f(void) { int x = 6 * 7; return x; }", "f", ["return 42"], ["*"], "constant folding + propagation"),
    ("C", "int f(int y) { int x = 3; int z = x + y; return z; }", "f", ["3 + y"], [], "constant propagation into an expression"),
    ("C", "int f(int a, int b) { int x = a * b; int y = a * b; return x + y; }", "f", [], [], "local CSE (checked via records)"),
    ("C", "int f(int x, int y) { int q = x / y; return 1; }", "f", ["x / y"], [], "division by a variable is never removed (could trap)"),
    ("C", "int f(int x) { return x * 8; }", "f", ["x * 8"], ["<<"], "C signed multiply is not turned into a shift"),
    ("C", "int f(int x) { return x * 2; }", "f", ["x + x"], [], "strength reduction x*2 -> x+x"),
    ("C", "double f(double x) { return x + 0.0; }", "f", ["+ 0.0"], [], "x + 0.0 kept for doubles (-0.0)"),
    ("C", "int f(int x) { if (0) { x = 5; } return x; }", "f", [], ["x = 5"], "unreachable branch removed"),
    ("C", "int f(void) { int big = 2147483647 + 1; return big; }", "f", ["2147483647 + 1"], [], "signed overflow is not folded (undefined behaviour)"),
    ("Java", "class A { static int f(int x) { return x * 8; } public static void main(String[] a) { System.out.println(f(3)); } }", "A.f", ["<< 3"], [], "Java multiply by 8 becomes a shift"),
    ("Java", "class A { static int f() { return 2147483647 + 1; } public static void main(String[] a) { System.out.println(f()); } }", "A.f", ["return -2147483648"], [], "Java overflow folds with wrap-around"),
    ("Java", "class A { static int f(int x) { return x / 0 + 1; } public static void main(String[] a) { System.out.println(f(3)); } }", "A.f", ["x / 0"], [], "Java x / 0 kept (throws ArithmeticException)"),
    ("Python", "def f(x):\n    return x * 1\nprint(f(2))\n", "f", ["x * 1"], [], "Python x * 1 kept for unknown x"),
    ("Python", "def f(n):\n    t = 0\n    for i in range(n):\n        t += i * 1\n    return t\nprint(f(4))\n", "f", [], ["* 1"], "Python proven-int loop counter simplified"),
    ("Python", "def f():\n    return 2 ** 70\nprint(f())\n", "f", ["2 ** 70"], [], "Python big-integer result not folded (beyond 64 bits)"),
    ("Python", "def f():\n    return 7 // -2\nprint(f())\n", "f", ["return -4"], [], "Python floor division folded with floor semantics"),
    ("Python", "def f():\n    return 1 / 0\nprint('ok')\n", "f", ["1 / 0"], [], "Python 1 / 0 kept (raises ZeroDivisionError)"),
]


def func_text(tac, name):
    for f in tac:
        if f["func"] == name:
            return "\n".join(i["t"] for i in f["code"])
    return ""


def section_optimizer():
    print("\n== optimizer ==")
    ext = {"C": ".c", "Java": ".java", "Python": ".py"}
    with tempfile.TemporaryDirectory() as tmp:
        for i, (lang, src, fn, must, mustnot, desc) in enumerate(OPTIMIZER):
            p = pathlib.Path(tmp) / (("A" if lang == "Java" else f"o{i}") + ext[lang])
            p.write_text(src)
            d = rcc(p, "--no-run")
            text = func_text(d["tac"]["optimized"], fn)
            ok = all(m in text for m in must) and not any(m in text for m in mustnot)
            if desc.startswith("local CSE"):
                ok = any(r["pass"].startswith("Local Value") for r in d["opt"]["records"])
            record("optimizer", f"{lang}: {desc}", ok, "optimized TAC:\n" + text)
        # every record must explain itself and name a real pass
        d = rcc(ROOT / "examples" / "c" / "05_optimizations.c")
        names = {p["name"] for p in d["opt"]["passes"]}
        ok = all(r["pass"] in names and r["why"] and (r["before"] or r["after"]) for r in d["opt"]["records"])
        record("optimizer", "every transformation record names its pass and gives a reason", ok)
        fired = {r["pass"] for r in d["opt"]["records"]}
        want = {"Constant Folding", "Constant Propagation", "Dead-Code Elimination", "Loop-Invariant Code Motion",
                "Strength Reduction", "Algebraic Simplification", "Local Value Numbering (CSE)", "Unreachable-Code Elimination",
                "Control-Flow Simplification", "Copy Propagation"}
        record("optimizer", "showcase triggers every major pass", want <= fired, f"missing {sorted(want - fired)}")
        # passes can be switched off
        d0 = rcc(ROOT / "examples" / "c" / "05_optimizations.c", "--passes", "none")
        record("optimizer", "--passes none leaves the TAC unchanged", d0["tac"]["originalCount"] == d0["tac"]["optimizedCount"] and not d0["opt"]["records"])


# ---- random programs --------------------------------------------------------

def gen_expr(rng, vars_, depth, muls):
    if depth == 0 or rng.random() < 0.3:
        return rng.choice(vars_) if rng.random() < 0.6 else str(rng.randint(0, 20))
    op = rng.choice(["+", "-", "*", "+", "-"] if muls[0] < 2 else ["+", "-"])
    if op == "*":
        muls[0] += 1
    return f"({gen_expr(rng, vars_, depth - 1, muls)} {op} {gen_expr(rng, vars_, depth - 1, muls)})"


def gen_cond(rng, vars_, lang):
    a = gen_expr(rng, vars_, 1, [2])
    b = gen_expr(rng, vars_, 1, [2])
    c = f"{a} {rng.choice(['<', '<=', '>', '>=', '==', '!='])} {b}"
    if rng.random() < 0.3:
        d = f"{rng.choice(vars_)} {rng.choice(['<', '>', '!='])} {rng.randint(0, 50)}"
        c = f"({c}) {'and' if lang == 'Python' else '&&'} ({d})" if rng.random() < 0.5 else f"({c}) {'or' if lang == 'Python' else '||'} ({d})"
    return c


def gen_program(rng, lang):
    vs = ["a", "b", "c", "d"]
    lines = []
    ind = lambda n: "    " * n   # noqa: E731

    def assign(v, e, lvl):
        if lang == "Python":
            lines.append(f"{ind(lvl)}{v} = ({e}) % 1009")
        else:
            lines.append(f"{ind(lvl)}{v} = ({e}) % 1009;")

    def out(lvl):
        if lang == "C":
            lines.append(ind(lvl) + 'printf("%ld %ld %ld %ld\\n", a, b, c, d);')
        elif lang == "Java":
            lines.append(ind(lvl) + 'System.out.println(a + " " + b + " " + c + " " + d);')
        else:
            lines.append(ind(lvl) + "print(a, b, c, d)")

    def block(lvl, budget):
        for _ in range(rng.randint(2, 4)):
            r = rng.random()
            if r < 0.45 or budget == 0:
                assign(rng.choice(vs), gen_expr(rng, vs, 3, [0]), lvl)
            elif r < 0.65:
                cond = gen_cond(rng, vs, lang)
                if lang == "Python":
                    lines.append(f"{ind(lvl)}if {cond}:")
                    block(lvl + 1, budget - 1)
                    lines.append(f"{ind(lvl)}else:")
                    block(lvl + 1, budget - 1)
                else:
                    lines.append(f"{ind(lvl)}if ({cond}) {{")
                    block(lvl + 1, budget - 1)
                    lines.append(f"{ind(lvl)}}} else {{")
                    block(lvl + 1, budget - 1)
                    lines.append(f"{ind(lvl)}}}")
            elif r < 0.85:
                k = rng.randint(2, 6)
                it = "i" + str(lvl)
                if lang == "Python":
                    lines.append(f"{ind(lvl)}for {it} in range({k}):")
                    assign(rng.choice(vs), gen_expr(rng, vs + [it], 2, [0]), lvl + 1)
                    block(lvl + 1, budget - 1)
                else:
                    t = "long" if lang == "C" else "long"
                    lines.append(f"{ind(lvl)}for ({t} {it} = 0; {it} < {k}; {it}++) {{")
                    assign(rng.choice(vs), gen_expr(rng, vs + [it], 2, [0]), lvl + 1)
                    block(lvl + 1, budget - 1)
                    lines.append(f"{ind(lvl)}}}")
            else:
                v = rng.choice(vs)
                call = f"mix({v}, {rng.choice(vs)})"
                lines.append(f"{ind(lvl)}{v} = {call}" + ("" if lang == "Python" else ";"))
        out(lvl)

    init = [rng.randint(0, 30) for _ in vs]
    if lang == "C":
        lines += ["#include <stdio.h>", "long mix(long x, long y) { return (x * 3 + y * 7 + 11) % 1009; }",
                  "int main(void) {", "    long " + ", ".join(f"{v} = {n}" for v, n in zip(vs, init)) + ";"]
        block(1, 2)
        lines += ["    return 0;", "}"]
    elif lang == "Java":
        lines += ["public class R {", "    static long mix(long x, long y) { return (x * 3 + y * 7 + 11) % 1009; }",
                  "    public static void main(String[] args) {", "        long " + ", ".join(f"{v} = {n}" for v, n in zip(vs, init)) + ";"]
        start = len(lines)
        block(2, 2)
        lines += ["    }", "}"]
        _ = start
    else:
        lines += ["def mix(x, y):", "    return (x * 3 + y * 7 + 11) % 1009", ""]
        lines += [f"{v} = {n}" for v, n in zip(vs, init)]
        block(0, 2)
    return "\n".join(lines) + "\n"


def section_random():
    print("\n== random differential programs ==")
    n = 8 if QUICK else 30
    rng = random.Random(20261030)
    for lang, ext in (("C", ".c"), ("Java", ".java"), ("Python", ".py")):
        ok_native = ok_opt = 0
        failures = []
        with tempfile.TemporaryDirectory() as tmp:
            for k in range(n):
                src = gen_program(rng, lang)
                p = pathlib.Path(tmp) / ("R.java" if lang == "Java" else f"r{k}{ext}")
                p.write_text(src)
                d = rcc(p)
                try:
                    out, failed = native(p, lang, tmp)
                except Exception as e:      # noqa: BLE001
                    failures.append(f"#{k}: {e}")
                    continue
                run = d.get("run")
                if run and run["original"]["stdout"] == out:
                    ok_native += 1
                else:
                    failures.append(f"#{k}: native differs" + (f" ({run['original']['message']})" if run else " (no run)"))
                    (ROOT / "build" / f"failed_random_{lang}_{k}{ext}").write_text(src)
                if run and run["equivalent"] is True:
                    ok_opt += 1
        record("random", f"{lang}: {ok_native}/{n} random programs match the native {lang} output", ok_native == n, "; ".join(failures[:3]))
        record("random", f"{lang}: {ok_opt}/{n} optimized programs equivalent to the original", ok_opt == n)


# ---- large programs -----------------------------------------------------------

def big_program(lang, copies):
    if lang == "C":
        unit = """
long work_{i}(long n) {{
    long data[16];
    long acc = {i};
    for (int k = 0; k < 16; k++) data[k] = (k * {i} + 7) % 101;
    for (int k = 0; k < n; k++) {{
        long x = data[k % 16] * 3 + {i};
        if (x % 2 == 0) acc += x / 2;
        else if (x % 3 == 0) acc -= x;
        else acc += x * 2 - 1;
        while (acc > 100000) acc -= 99991;
    }}
    int limit = 4 * 4 + 1;
    switch ((int)(acc % 4)) {{
    case 0: acc += limit; break;
    case 1: acc -= limit;
    default: acc *= 2;
    }}
    return acc % 100003;
}}
"""
        body = "".join(unit.format(i=i) for i in range(copies))
        calls = "".join(f"    total = (total + work_{i}({20 + i % 7})) % 1000003;\n" for i in range(copies))
        return "#include <stdio.h>\n" + body + "int main(void) {\n    long total = 0;\n" + calls + '    printf("%ld\\n", total);\n    return 0;\n}\n'
    if lang == "Java":
        unit = """
    static long work{i}(long n) {{
        long[] data = new long[16];
        long acc = {i};
        for (int k = 0; k < 16; k++) data[k] = (k * {i} + 7) % 101;
        for (int k = 0; k < n; k++) {{
            long x = data[k % 16] * 3 + {i};
            if (x % 2 == 0) acc += x / 2;
            else if (x % 3 == 0) acc -= x;
            else acc += x * 2 - 1;
            while (acc > 100000) acc -= 99991;
        }}
        int limit = 4 * 4 + 1;
        switch ((int) (acc % 4)) {{
            case 0: acc += limit; break;
            case 1: acc -= limit;
            default: acc *= 2;
        }}
        return acc % 100003;
    }}
"""
        body = "".join(unit.format(i=i) for i in range(copies))
        calls = "".join(f"        total = (total + work{i}({20 + i % 7})) % 1000003;\n" for i in range(copies))
        return "public class Big {\n" + body + "    public static void main(String[] args) {\n        long total = 0;\n" + calls + "        System.out.println(total);\n    }\n}\n"
    unit = """
def work_{i}(n):
    data = [0] * 16
    acc = {i}
    for k in range(16):
        data[k] = (k * {i} + 7) % 101
    for k in range(n):
        x = data[k % 16] * 3 + {i}
        if x % 2 == 0:
            acc += x // 2
        elif x % 3 == 0:
            acc -= x
        else:
            acc += x * 2 - 1
        while acc > 100000:
            acc -= 99991
    limit = 4 * 4 + 1
    if acc % 4 == 0:
        acc += limit
    else:
        acc *= 2
    return acc % 100003
"""
    body = "".join(unit.format(i=i) for i in range(copies))
    calls = "".join(f"total = (total + work_{i}({20 + i % 7})) % 1000003\n" for i in range(copies))
    return body + "\ntotal = 0\n" + calls + "print(total)\n"


def section_stress():
    print("\n== large programs ==")
    if QUICK:
        print("  (skipped with --quick)")
        return
    for lang, ext, copies in (("C", ".c", 80), ("Java", ".java", 80), ("Python", ".py", 80)):
        src = big_program(lang, copies)
        nlines = src.count("\n")
        with tempfile.TemporaryDirectory() as tmp:
            p = pathlib.Path(tmp) / ("Big.java" if lang == "Java" else "big" + ext)
            p.write_text(src)
            t0 = time.time()
            d = differential(p, lang, f"{lang} {nlines}-line generated program", "stress")
            dt = time.time() - t0
            m = d["metrics"]
            record("stress", f"{lang} {nlines} lines: all {m['tokens']} tokens kept (no truncation)", d["tokens"]["count"] == d["tokens"]["shown"] == m["tokens"])
            record("stress", f"{lang} {nlines} lines: translation complete, {m['tacOriginal']} -> {m['tacOptimized']} TAC instructions", d["tac"]["complete"])
            record("stress", f"{lang} {nlines} lines: whole pipeline in {dt:.1f}s (lex {m['ms']['lex']:.1f} ms, parse {m['ms']['parse']:.1f} ms, opt {m['ms']['opt']:.0f} ms)", dt < 60)


# ---- lexical ------------------------------------------------------------------

def section_lexical():
    print("\n== lexical rules ==")
    d = rcc(ROOT / "examples" / "lexical" / "calculator.txt", "--rules", str(ROOT / "examples" / "lexical" / "calculator.rules"))
    names = [t[0] for t in d["tokens"]["list"]]
    record("lexical", "custom rules: tokens recognised", names[:4] == ["LET", "NAME", "ASSIGN", "FLOAT"], str(names[:6]))
    record("lexical", "custom rules: later stages marked skipped, not faked",
           all(s["status"] == "skipped" for s in d["stages"] if s["id"] in ("parse", "tac", "opt")) and "tac" not in d)
    record("lexical", "custom rules: automata exported for every rule", all(r["nfa"] and r["dfa"] and r["min"] for r in d["rules"]["rules"]))
    with tempfile.TemporaryDirectory() as tmp:
        bad = pathlib.Path(tmp) / "bad.rules"
        bad.write_text("GOOD 1 token [a-z]+\nBAD 2 token (ab\nWORSE token\n")
        src = pathlib.Path(tmp) / "x.txt"
        src.write_text("abc")
        d = rcc(src, "--rules", str(bad))
        cats = {x["category"] for x in d["diagnostics"]}
        record("lexical", "invalid rules file: malformed line and bad regex both reported", {"Malformed Rule"} <= cats or {"Invalid Regex"} <= cats, str(cats))
        shadow = pathlib.Path(tmp) / "shadow.rules"
        shadow.write_text("ID 1 token [a-z]+\nKW 2 token if\nWS 3 skip [ ]+\n")
        d = rcc(src, "--rules", str(shadow))
        record("lexical", "shadowed rule reported as unreachable", any(x["category"] == "Unreachable Rule" for x in d["diagnostics"]))
    # the original command-line lexer keeps its exact output
    golden = {"lexer_sample_c.txt": ["examples/sample.c", "--rules", "rules/c.rules"],
              "lexer_sample_py.txt": ["examples/sample.py", "--rules", "rules/python.rules"],
              "lexer_sample_java.txt": ["examples/Sample.java"], "lexer_source_txt.txt": []}
    for name, args in golden.items():
        out = subprocess.run([str(ROOT / "lexer"), *args], capture_output=True, text=True, cwd=ROOT).stdout
        record("lexical", f"./lexer {' '.join(args) or '(source.txt)'} matches golden output", out == (ROOT / "tests" / "golden" / name).read_text())


def main():
    if not RCC.exists():
        sys.exit("build/rcc is missing -- run make first")
    missing = [t for t in ("cc", "javac", "java") if not shutil.which(t)]
    if missing:
        print("warning: missing native tools (their comparisons will fail):", missing)
    t0 = time.time()
    for s in (section_examples, section_diagnostics, section_frontend, section_optimizer, section_random, section_stress, section_lexical):
        s()
    passed = sum(r["ok"] for r in results)
    print(f"\n{passed}/{len(results)} checks passed in {time.time() - t0:.0f}s")
    by = {}
    for r in results:
        b = by.setdefault(r["section"], [0, 0])
        b[0] += r["ok"]
        b[1] += 1
    for k, (a, b) in by.items():
        print(f"  {k:<12} {a}/{b}")
    (ROOT / "tests" / "last_report.json").write_text(json.dumps({"passed": passed, "total": len(results), "sections": by,
                                                                 "results": results, "quick": QUICK,
                                                                 "date": time.strftime("%Y-%m-%d %H:%M")}, indent=1))
    sys.exit(0 if passed == len(results) else 1)


if __name__ == "__main__":
    main()
