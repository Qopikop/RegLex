/* The workbench without a server (GitHub Pages): the same three API calls as visualize/serve.py.
   "wasm" mode: the real compiler built to WebAssembly (web/build_wasm.sh) runs in a worker (rcc-worker.js);
     compile() and simulate() build the same rcc arguments serve.py does; detect() mirrors visualize/detect.py.
   "examples" mode (no wasm build): the examples' real compiler output, saved by web/build_pages.py. */
"use strict";
const BrowserCompiler = (() => {
  const MAX_CODE_BYTES = 1_000_000, TIMEOUT_MS = 60_000;
  const LANGS = { C: ".c", Java: ".java", Python: ".py" };
  let mode = null;
  const RULE_FILE = { C: "/rules/c.rules", Java: "/rules/java.rules", Python: "/rules/python.rules" };

  /* ---- language detection (visualize/detect.py) ---- */
  const FEATURES = {
    C: [[/^\s*#\s*(include|define|ifndef|ifdef|pragma)\b/gm, 5, "#include / #define"], [/\bint\s+main\s*\(/gm, 4, "int main("],
        [/\b(printf|scanf|fprintf|puts)\s*\(/gm, 3, "printf / scanf"], [/\b(malloc|calloc|realloc|free|sizeof)\s*\(/gm, 3, "malloc / sizeof"],
        [/\bchar\s*\*/gm, 3, "char *"], [/\bstruct\s+\w+/gm, 2, "struct"], [/\w->\w/gm, 1, "-> member access"], [/\b(unsigned|typedef)\b/gm, 2, "unsigned / typedef"]],
    Python: [[/^\s*def\s+\w+\s*\(.*\)\s*(->\s*[^:]+)?:\s*(#.*)?$/gm, 5, "def ...:"], [/^\s*from\s+[\w.]+\s+import\b/gm, 5, "from ... import"],
        [/^\s*import\s+[\w.]+(\s+as\s+\w+)?\s*(#.*)?$/gm, 3, "import without ;"], [/\bself\b/gm, 3, "self"],
        [/\b(elif|None|True|False|lambda|nonlocal)\b/gm, 3, "elif / None / True"], [/^\s*(if|elif|else|for|while|class|with|try|except|finally)\b[^;{\n]*:\s*(#.*)?$/gm, 2, "block ending in ':'"],
        [/\bprint\s*\(/gm, 2, "print("], [/"""|'''/gm, 3, "triple-quoted string"], [/^\s*@\w+/gm, 1, "decorator"]],
    Java: [[/\b(public|private|protected)\s+(static\s+)?(final\s+)?(class|interface|enum|void|int|String)\b/gm, 5, "public class / void"],
        [/\bSystem\.(out|err)\.print/gm, 5, "System.out.print"], [/^\s*import\s+java[x]?\./gm, 5, "import java."], [/^\s*package\s+[\w.]+\s*;/gm, 4, "package ...;"],
        [/\bString\s*\[\]/gm, 4, "String[]"], [/@Override\b/gm, 3, "@Override"], [/\bnew\s+[A-Z]\w*\s*[(<\[]/gm, 1, "new ClassName("]],
    JavaScript: [[/\bconsole\.(log|error|warn)\s*\(/gm, 5, "console.log"], [/\b(const|let)\s+[\w$]+\s*=/gm, 3, "const / let"], [/=>/gm, 2, "arrow function =>"],
        [/\bfunction\s*[\w$]*\s*\(/gm, 3, "function("], [/===|!==/gm, 3, "=== / !=="], [/\brequire\s*\(|^\s*export\s+(default|const|function|class)\b/gm, 4, "require / export"],
        [/`[^`]*\$\{/gm, 3, "template literal ${...}"], [/\b(document|window)\.\w+/gm, 3, "document / window"], [/\bundefined\b/gm, 2, "undefined"]],
  };
  const EXTENSIONS = { ".c": "C", ".h": "C", ".py": "Python", ".pyw": "Python", ".java": "Java", ".js": "JavaScript", ".mjs": "JavaScript", ".cjs": "JavaScript", ".jsx": "JavaScript" };
  function detect(code, filename) {
    const dot = filename ? filename.lastIndexOf(".") : -1, ext = dot >= 0 ? filename.slice(dot).toLowerCase() : "";
    if (EXTENSIONS[ext]) return { language: EXTENSIONS[ext], reason: `file extension ${ext}` };
    const scores = {}, evidence = {};
    for (const [lang, feats] of Object.entries(FEATURES)) {
      let total = 0; const hits = [];
      for (const [re, weight, desc] of feats) { const n = (code.match(re) || []).length; if (n) { total += weight * Math.min(n, 3); hits.push(desc); } }
      scores[lang] = total; evidence[lang] = hits;
    }
    const semis = (code.match(/;\s*(\/\/.*)?$/gm) || []).length;   // statements ending in ';' count against Python
    if (semis >= 2) scores.Python -= 2 * Math.min(semis, 5);
    const best = Object.keys(scores).reduce((a, b) => scores[b] > scores[a] ? b : a);
    if (scores[best] < 3) return { language: "Other", reason: "no language stood out, so the general rules are used" };
    return { language: best, reason: "found " + evidence[best].slice(0, 4).join(", ") };
  }

  /* ---- the worker ---- */
  let worker = null, seq = 0;
  const pending = new Map();
  function getWorker() {
    if (worker) return worker;
    worker = new Worker("rcc-worker.js");
    worker.onmessage = e => { const p = pending.get(e.data.id); if (p) { pending.delete(e.data.id); clearTimeout(p.timer); p.resolve(e.data); } };
    worker.onerror = e => { for (const p of pending.values()) p.reject(new Error(e.message || "the browser compiler failed to start")); pending.clear(); worker = null; };
    return worker;
  }
  function run(args, files) {
    return new Promise((resolve, reject) => {
      const id = ++seq;
      const timer = setTimeout(() => {   // a runaway program: stop the worker and start a fresh one next time
        pending.delete(id); worker?.terminate(); worker = null;
        reject(new Error(`The compiler took longer than ${TIMEOUT_MS / 1000} s and was stopped.`));
      }, TIMEOUT_MS);
      pending.set(id, { resolve, reject, timer });
      getWorker().postMessage({ id, args, files });
    });
  }

  /* ---- examples mode: saved results, looked up by the exact source text ---- */
  const LOCAL_ONLY = "To compile your own code, run the workbench on your computer (./run.sh).";
  let index = null;
  async function saved(req) {
    if (Array.isArray(req.passes)) throw new Error("This online demo shows the examples with every optimization pass on. " + LOCAL_ONLY);
    const bytes = new TextEncoder().encode((req.code ?? "") + "\0" + (req.rules || ""));
    const hash = [...new Uint8Array(await crypto.subtle.digest("SHA-256", bytes))].map(b => b.toString(16).padStart(2, "0")).join("");
    index ||= await fetch("results/index.json").then(r => r.json());
    if (!index[hash]) throw new Error("This online demo compiles the built-in examples only (Open → Load example). " + LOCAL_ONLY);
    const data = await fetch("results/" + index[hash]).then(r => r.json());
    if (req.filename) data.source.name = req.filename;
    return data;
  }

  /* ---- POST /api/compile ---- */
  async function compile(req) {
    if (mode === "examples") return saved(req);
    const code = req.code ?? "";
    if (!code.trim()) throw new Error("Type, paste or open some code first.");
    if (new TextEncoder().encode(code).length > MAX_CODE_BYTES) throw new Error(`That code is too large. The limit is ${MAX_CODE_BYTES / 1000} KB.`);
    let lang = req.language || "auto", detected = null;
    if (lang === "auto") {
      const found = detect(code, req.filename);
      lang = LANGS[found.language] ? found.language : "C";
      detected = LANGS[found.language] ? found.reason : "no language stood out; using C";
    }
    if (!LANGS[lang]) throw new Error(`unknown language '${lang}'`);
    const name = lang === "Java" ? (code.match(/public\s+(?:final\s+|abstract\s+)*class\s+(\w+)/)?.[1] || "Main") + ".java" : "program" + LANGS[lang];
    const files = { ["/work/" + name]: code };
    const args = ["/work/" + name, "--json", "--rules-dir", "/rules", "--lang", lang];
    if (req.rules) {
      if (req.rules.length > 50_000) throw new Error("The rules are too long.");
      files["/work/custom.rules"] = req.rules;
      args.push("--rules", "/work/custom.rules", "--lex-only");
    }
    if (Array.isArray(req.passes)) args.push("--passes", req.passes.filter(p => typeof p === "string" && /^[A-Za-z]+$/.test(p)).join(",") || "none");
    if (code.length > 300_000) args.push("--max-tokens", "40000");
    const r = await run(args, files);
    if (!r.out.trim()) throw new Error(r.error || r.err.trim() || "the compiler produced no output");
    const data = JSON.parse(r.out);
    data.source.name = req.filename || name;
    if (detected) data.source.languageReason = detected;
    return data;
  }

  /* ---- POST /api/simulate ---- */
  async function simulate(req) {
    if (mode === "examples") throw new Error("Step-by-step simulation needs the full workbench. " + LOCAL_ONLY);
    const auto = ["nfa", "dfa", "min"].includes(req.automaton) ? req.automaton : "dfa";
    const lang = LANGS[req.language] ? req.language : "C";
    const files = {};
    let rules = RULE_FILE[lang];
    if (req.rules) { rules = "/work/custom.rules"; files[rules] = String(req.rules).slice(0, 50_000); }
    const r = await run(["--simulate", String(req.rule ?? ""), "--input", String(req.input ?? "").slice(0, 200), "--automaton", auto, "--rules", rules], files);
    if (r.error || r.code) throw new Error(r.err.trim() || r.error || "simulation failed");
    return JSON.parse(r.out);
  }

  /* ---- GET /api/examples: written at build time by web/build_wasm.sh ---- */
  async function examples() {
    const res = await fetch("examples.json");
    if (!res.ok) throw new Error("examples.json is missing");
    return res.json();
  }

  return {
    async call(path, body) {
      if (path === "api/compile") return compile(body);
      if (path === "api/simulate") return simulate(body);
      if (path === "api/examples") return examples();
      throw new Error("not available without the server: " + path);
    },
    /* "wasm", "examples", or null when neither is published next to the page */
    async available() {
      const ok = url => fetch(url, { method: "HEAD" }).then(r => r.ok, () => false);
      mode = typeof WebAssembly === "object" && await ok("wasm/rcc.wasm") ? "wasm" : await ok("results/index.json") ? "examples" : null;
      return mode;
    },
    get mode() { return mode; },
  };
})();
