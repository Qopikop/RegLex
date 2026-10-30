# RegLex Compiler Workbench

A compiler written in C, built around a regex-driven lexer generator:
regex → postfix → Thompson NFA → subset-construction DFA → minimized DFA →
scanner → recursive-descent parser (C, Java, Python) → AST + semantic checks →
three-address code → optimizer → TAC interpreter that checks the optimizer.
A web workbench shows every stage.

## Run

```bash
./run.sh                     # builds if needed, starts the workbench and opens it in the browser
make                         # or build only: build/rcc and ./lexer
python3 visualize/serve.py   # or start the server yourself (first free port from 8000)
```

Command line:

```bash
./build/rcc examples/c/04_toolkit.c          # readable report
./build/rcc examples/python/toolkit.py --json
./build/rcc prog.java --passes constprop,fold,dce
./build/rcc code.txt --rules my.rules --lex-only
./build/rcc --simulate IDENT --input abc_1 --rules rules/c.rules --automaton nfa
./lexer examples/sample.c --rules rules/c.rules   # original lexer CLI, same output as before
```

Tests: `make test` (automata unit tests + `tests/run_tests.py`); `make sanitize` builds an ASan/UBSan binary.

## What each language supports

| Stage | C | Java | Python |
|---|---|---|---|
| Lexing (DFA from `rules/*.rules`) | full | full | full, plus INDENT/DEDENT layout pass |
| Parsing | functions, globals, structs/enums/typedefs, all statements & operators, `#define` constants | classes, methods, fields, generics in types, all statements, try/catch, lambdas | functions, classes, all statements, comprehensions, f-strings, slices, decorators |
| TAC + execution | scalars, arrays (1-D/2-D), strings, recursion, switch, printf | static methods & fields, arrays, Strings, Math/Arrays/printf | ints/floats/str/lists/tuples, range/enumerate loops, defaults & keyword args, common builtins |
| Kept as **opaque** (reported, never dropped) | struct fields, pointer arithmetic, `&`, malloc | objects, instance fields, interfaces, exception handlers | dicts, attributes, comprehensions, lambdas, try/except, `with` |

Optimizer passes: constant propagation, constant folding, copy propagation, algebraic
simplification, strength reduction, local value numbering (CSE), global CSE,
loop-invariant code motion, dead-code elimination, unreachable-code elimination,
control-flow simplification. Each pass checks language rules before it acts
(C signed overflow is not folded, Java wraps, Python only rewrites proven ints).

## Layout

| Path | What |
|---|---|
| `src/` | the compiler: `regex` `automata` `lexer` `parse*` `ast` `lower` (TAC + semantics) `opt` `value` (language arithmetic) `interp` `rcc` (driver) |
| `rules/` | lexical rules for C, Java, Python and the general set |
| `examples/` | basic → advanced programs per language, error and partial-support demos (`manifest.json`) |
| `web/` | the workbench: `index.html`, `workbench.css`, `app.js` |
| `run.sh` | one-command launcher |
| `index.html`, `visualize/lexer_visualizer.html`, `visualize/nfa.html` | the earlier static lexer visualizer, kept for GitHub Pages (the workbench needs the local server) |
| `visualize/` | local server (`serve.py`) and language detection |
| `tests/` | unit, differential, random, stress and golden tests |
