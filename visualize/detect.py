"""Guesses the language of a piece of source code, so the page can pick fixed rules for it.

No rules are generated: detection only chooses between the rule sets that
already exist (rules/c.rules, rules/python.rules, or the built-in general
rules in main.c). Each language scores points for features that are typical
of it; the highest score wins, and a weak score falls back to the general rules.
"""
import pathlib
import re

M = re.MULTILINE

# language -> (rules file or None for the built-in general rules, [(pattern, weight, description)])
LANGUAGES = {
    "C": ("rules/c.rules", [
        (r"^\s*#\s*(include|define|ifndef|ifdef|pragma)\b", 5, "#include / #define"),
        (r"\bint\s+main\s*\(", 4, "int main("),
        (r"\b(printf|scanf|fprintf|puts)\s*\(", 3, "printf / scanf"),
        (r"\b(malloc|calloc|realloc|free|sizeof)\s*\(", 3, "malloc / sizeof"),
        (r"\bchar\s*\*", 3, "char *"),
        (r"\bstruct\s+\w+", 2, "struct"),
        (r"\w->\w", 1, "-> member access"),
        (r"\b(unsigned|typedef)\b", 2, "unsigned / typedef"),
    ]),
    "Python": ("rules/python.rules", [
        (r"^\s*def\s+\w+\s*\(.*\)\s*(->\s*[^:]+)?:\s*(#.*)?$", 5, "def ...:"),
        (r"^\s*from\s+[\w.]+\s+import\b", 5, "from ... import"),
        (r"^\s*import\s+[\w.]+(\s+as\s+\w+)?\s*(#.*)?$", 3, "import without ;"),
        (r"\bself\b", 3, "self"),
        (r"\b(elif|None|True|False|lambda|nonlocal)\b", 3, "elif / None / True"),
        (r"^\s*(if|elif|else|for|while|class|with|try|except|finally)\b[^;{\n]*:\s*(#.*)?$", 2, "block ending in ':'"),
        (r"\bprint\s*\(", 2, "print("),
        (r'"""|\'\'\'', 3, "triple-quoted string"),
        (r"^\s*@\w+", 1, "decorator"),
    ]),
    "Java": (None, [
        (r"\b(public|private|protected)\s+(static\s+)?(final\s+)?(class|interface|enum|void|int|String)\b", 5, "public class / void"),
        (r"\bSystem\.(out|err)\.print", 5, "System.out.print"),
        (r"^\s*import\s+java[x]?\.", 5, "import java."),
        (r"^\s*package\s+[\w.]+\s*;", 4, "package ...;"),
        (r"\bString\s*\[\]", 4, "String[]"),
        (r"@Override\b", 3, "@Override"),
        (r"\bnew\s+[A-Z]\w*\s*[(<\[]", 1, "new ClassName("),
    ]),
    "JavaScript": (None, [
        (r"\bconsole\.(log|error|warn)\s*\(", 5, "console.log"),
        (r"\b(const|let)\s+[\w$]+\s*=", 3, "const / let"),
        (r"=>", 2, "arrow function =>"),
        (r"\bfunction\s*[\w$]*\s*\(", 3, "function("),
        (r"===|!==", 3, "=== / !=="),
        (r"\brequire\s*\(|^\s*export\s+(default|const|function|class)\b", 4, "require / export"),
        (r"`[^`]*\$\{", 3, "template literal ${...}"),
        (r"\b(document|window)\.\w+", 3, "document / window"),
        (r"\bundefined\b", 2, "undefined"),
    ]),
}

EXTENSIONS = {".c": "C", ".h": "C", ".py": "Python", ".pyw": "Python", ".java": "Java",
              ".js": "JavaScript", ".mjs": "JavaScript", ".cjs": "JavaScript", ".jsx": "JavaScript"}

MIN_SCORE = 3   # below this nothing is convincing, so use the general rules


def rules_for(language):
    return LANGUAGES[language][0] if language in LANGUAGES else None


def detect(code, filename=None):
    """Returns {"language", "rules", "reason", "scores"}."""
    ext = pathlib.Path(filename).suffix.lower() if filename else ""
    if ext in EXTENSIONS:
        lang = EXTENSIONS[ext]
        return {"language": lang, "rules": rules_for(lang), "reason": f"file extension {ext}", "scores": {}}

    scores, evidence = {}, {}
    for lang, (_, features) in LANGUAGES.items():
        total, hits = 0, []
        for pattern, weight, desc in features:
            n = len(re.findall(pattern, code, M))
            if n:
                total += weight * min(n, 3)      # cap so one repeated feature can't dominate
                hits.append(desc)
        scores[lang], evidence[lang] = total, hits

    # Statements ending in ';' are strong evidence against Python.
    semicolon_lines = len(re.findall(r";\s*(//.*)?$", code, M))
    if semicolon_lines >= 2:
        scores["Python"] -= 2 * min(semicolon_lines, 5)

    best = max(scores, key=scores.get)
    if scores[best] < MIN_SCORE:
        return {"language": "Other", "rules": None, "scores": scores,
                "reason": "no language stood out, so the general rules are used"}
    return {"language": best, "rules": rules_for(best), "scores": scores,
            "reason": "found " + ", ".join(evidence[best][:4])}
