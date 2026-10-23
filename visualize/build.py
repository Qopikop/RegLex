"""Builds visualize/lexer_visualizer.html: the lexer's output as an interactive page.

Compiles visualize/trace_json.c (which #includes main.c), runs it on each
program, and embeds every trace in template.html. The page has a switcher
when it holds more than one program. serve.py imports these functions to
lex code typed or pasted into the page.

Usage, from the project root:
    python3 visualize/build.py --open                  # demo set: every program in examples/, then open it
    python3 visualize/build.py prog.py                 # one program, general rules
    python3 visualize/build.py prog.py --rules rules/python.rules --open
"""
import json
import pathlib
import subprocess
import sys
import webbrowser

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
TRACER = HERE / "trace_json"

# (source file, rules file or None for the built-in general rules)
DEMO_SET = [
    ("examples/sample.c", "rules/c.rules"),
    ("examples/sample.py", "rules/python.rules"),
    ("examples/Sample.java", None),
    ("examples/sample.js", None),
]


def compile_tracer():
    subprocess.run(["cc", "-O2", "-o", str(TRACER), str(HERE / "trace_json.c")], check=True)


def trace(source, rules, label=None, timeout=20):
    """Runs the real lexer on `source` and returns {"label", "trace"}. Raises RuntimeError on failure."""
    args = [str(TRACER), str(source)] + (["--rules", rules] if rules else [])
    result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or f"trace_json failed on {source}")
    data = json.loads(result.stdout)  # fail loudly if the C side ever emits bad JSON
    if len(data["source"]) > 60_000:
        print(f"note: {source} is {len(data['source']):,} bytes; one cell per character "
              "may make the page slow to open", file=sys.stderr)
    if label is None:
        label = pathlib.Path(rules).stem.capitalize() + " rules" if rules else "general rules"
    return {"label": label, "trace": data}


def page_html(runs):
    page = (HERE / "template.html").read_text()
    return page.replace("__TRACE__", json.dumps(runs, ensure_ascii=False).replace("</", "<\\/"))


def set_key(rules_source):
    """Short id for a rule set, used in links like nfa.html#c-IDENT (same mapping as the page's JS)."""
    if rules_source == "built-in general rules":
        return "general"
    return pathlib.Path(rules_source).stem.lower()


def nfa_data(runs):
    """One entry per distinct rule set in `runs`, plus the single-pattern demo."""
    sets = {}
    for run in runs:
        t = run["trace"]
        key = set_key(t["rulesSource"])
        entry = sets.setdefault(key, {
            "label": "General rules" if key == "general" else key.capitalize() + " rules",
            "source": t["rulesSource"], "users": [], "rules": [dict(r) for r in t["rules"]],
        })
        entry["users"].append(t["sourceName"].split("/")[-1])
        # A real example per rule: the longest short text it matched in the sample programs.
        for step in t["steps"]:
            if step["kind"] == "error":
                continue
            rule = entry["rules"][step["rule"]]
            text = t["source"][step["pos"]:step["pos"] + step["len"]]
            if len(text) <= 32 and "\n" not in text and len(text) > len(rule.get("example", "")):
                rule["example"] = text
    for entry in sets.values():
        entry["usedBy"] = "used by " + ", ".join(entry.pop("users"))
    demo = runs[0]["trace"]["demo"]
    sets["patterns"] = {
        "label": "Single patterns", "source": "Part of main.c's demo", "usedBy": "small examples",
        "rules": [{"name": d["pattern"], "pattern": d["pattern"], "postfix": d["postfix"], "nfa": d["nfa"],
                   # prefer an input that only partly matches: it shows the "longest start" idea
                   "example": next((r["input"] for r in d["results"] if r["len"] > 0 and not r["match"]),
                                   max((r["input"] for r in d["results"] if r["match"]), key=len, default=d["results"][0]["input"]))}
                  for d in demo],
    }
    return {"sets": sets}


def nfa_page_html(runs):
    page = (HERE / "nfa_template.html").read_text()
    return page.replace("__NFA_DATA__", json.dumps(nfa_data(runs), ensure_ascii=False).replace("</", "<\\/"))


def demo_runs():
    return [trace(src, rules) for src, rules in DEMO_SET]


def main(args):
    open_page = "--open" in args
    args = [a for a in args if a != "--open"]
    compile_tracer()
    try:
        if args:
            rules = args[args.index("--rules") + 1] if "--rules" in args else None
            sources = [a for i, a in enumerate(args) if a != "--rules" and (i == 0 or args[i - 1] != "--rules")]
            if len(sources) != 1:
                sys.exit("usage: python3 visualize/build.py [source-file [--rules rules-file]] [--open]")
            runs = [trace(sources[0], rules)]
        else:
            runs = demo_runs()
    except RuntimeError as e:
        sys.exit(str(e))

    out = HERE / "lexer_visualizer.html"
    out.write_text(page_html(runs))
    (HERE / "nfa.html").write_text(nfa_page_html(runs))
    for r in runs:
        t = r["trace"]
        errors = sum(s["kind"] == "error" for s in t["steps"])
        print(f"  {t['sourceName']:<22} {r['label']:<15} {len(t['steps']):>5} steps, {errors} errors")
    print(f"wrote {out} and {HERE / 'nfa.html'}")
    if open_page:
        webbrowser.open(out.as_uri())


if __name__ == "__main__":
    main(sys.argv[1:])
