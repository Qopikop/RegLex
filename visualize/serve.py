"""Local server for the compiler workbench (web/index.html).

  GET  /                 the workbench (plus web/workbench.css and web/app.js)
  POST /api/compile      {code, language, filename?, rules?, passes?} -> build/rcc --json
  POST /api/simulate     {rule, input, automaton, language, rules?}  -> build/rcc --simulate
  GET  /api/examples     the example library (examples/manifest.json + sources)

The real C compiler (build/rcc) does all the work; this file only passes text
in and JSON out. Each run gets a temporary directory, fixed argument lists (no
shell), a time limit and an input size limit. It listens on 127.0.0.1 only.

Usage, from the project root:
    python3 visualize/serve.py            # builds if needed, opens http://127.0.0.1:8000/
    python3 visualize/serve.py --port 9000 --no-open
"""
import json
import pathlib
import subprocess
import sys
import tempfile
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import detect  # noqa: E402  (same directory)

MAX_CODE_BYTES = 1_000_000
ROOT = pathlib.Path(__file__).resolve().parent.parent
RCC = ROOT / "build" / "rcc"
WEB = ROOT / "web" / "index.html"
STATIC = {"/workbench.css": "text/css; charset=utf-8", "/app.js": "text/javascript; charset=utf-8"}
COMPILE_TIMEOUT = 60
WORKBENCH_LANGS = {"C": ".c", "Java": ".java", "Python": ".py"}


def java_file_name(code):
    """javac-style: a public class must live in <Name>.java; rcc only needs the extension."""
    import re
    m = re.search(r"public\s+(?:final\s+|abstract\s+)*class\s+(\w+)", code)
    return (m.group(1) if m else "Main") + ".java"


def compile_code(req):
    code = req.get("code", "")
    if not isinstance(code, str) or not code.strip():
        raise ValueError("Type, paste or open some code first.")
    if len(code.encode("utf-8")) > MAX_CODE_BYTES:
        raise ValueError(f"That code is too large. The limit is {MAX_CODE_BYTES // 1000} KB.")
    lang = req.get("language") or "auto"
    filename = req.get("filename")
    rules_text = req.get("rules")
    detected = None
    if lang == "auto":
        found = detect.detect(code, filename)
        lang = found["language"] if found["language"] in WORKBENCH_LANGS else "C"
        detected = found["reason"] if found["language"] in WORKBENCH_LANGS else "no language stood out; using C"
    if lang not in WORKBENCH_LANGS:
        raise ValueError(f"unknown language {lang!r}")
    args = [str(RCC)]
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        name = java_file_name(code) if lang == "Java" else "program" + WORKBENCH_LANGS[lang]
        src = tmp / name
        src.write_text(code, encoding="utf-8")
        args += [str(src), "--json", "--rules-dir", str(ROOT / "rules"), "--lang", lang]
        if rules_text:
            if len(rules_text) > 50_000:
                raise ValueError("The rules are too long.")
            (tmp / "custom.rules").write_text(rules_text, encoding="utf-8")
            args += ["--rules", str(tmp / "custom.rules"), "--lex-only"]
        passes = req.get("passes")
        if isinstance(passes, list):
            args += ["--passes", ",".join(p for p in passes if isinstance(p, str) and p.isalpha()) or "none"]
        if len(code) > 300_000:
            args += ["--max-tokens", "40000"]
        r = subprocess.run(args, capture_output=True, text=True, timeout=COMPILE_TIMEOUT, cwd=tmp)
    if not r.stdout.strip():
        raise RuntimeError(r.stderr.strip() or "the compiler produced no output")
    data = json.loads(r.stdout)
    data["source"]["name"] = filename or name
    if detected:
        data["source"]["languageReason"] = detected
    return data


def simulate(req):
    rule = str(req.get("rule", ""))
    text = str(req.get("input", ""))[:200]
    auto = req.get("automaton") if req.get("automaton") in ("nfa", "dfa", "min") else "dfa"
    lang = req.get("language") if req.get("language") in WORKBENCH_LANGS else "C"
    with tempfile.TemporaryDirectory() as tmp:
        rules = ROOT / "rules" / ({"C": "c", "Java": "java", "Python": "python"}[lang] + ".rules")
        if req.get("rules"):
            rules = pathlib.Path(tmp) / "custom.rules"
            rules.write_text(str(req["rules"])[:50_000], encoding="utf-8")
        r = subprocess.run([str(RCC), "--simulate", rule, "--input", text, "--automaton", auto, "--rules", str(rules)],
                           capture_output=True, text=True, timeout=20)
    if r.returncode:
        raise ValueError(r.stderr.strip() or "simulation failed")
    return json.loads(r.stdout)


def examples():
    m = json.loads((ROOT / "examples" / "manifest.json").read_text())
    for ex in m["examples"]:
        ex["code"] = (ROOT / "examples" / ex["file"]).read_text()
    for ex in m["lexical"]:
        ex["code"] = (ROOT / "examples" / ex["file"]).read_text()
        ex["rulesText"] = (ROOT / "examples" / ex["rules"]).read_text()
    m["defaultRules"] = {k: (ROOT / "rules" / f"{v}.rules").read_text() for k, v in (("C", "c"), ("Java", "java"), ("Python", "python"))}
    report = ROOT / "tests" / "last_report.json"
    if report.exists():
        r = json.loads(report.read_text())
        m["tests"] = {"passed": r["passed"], "total": r["total"], "sections": r["sections"], "date": r.get("date")}
    return m


class Handler(BaseHTTPRequestHandler):
    def _send(self, status, body, ctype="application/json; charset=utf-8"):
        data = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = self.path.split("?")[0].split("#")[0]
        if path in ("/", "/index.html"):
            self._send(200, WEB.read_bytes(), "text/html; charset=utf-8")
        elif path in STATIC:                  # the workbench's own stylesheet and script
            self._send(200, (WEB.parent / path[1:]).read_bytes(), STATIC[path])
        elif path == "/api/examples":
            self._send(200, examples())
        elif path == "/api/health":
            self._send(200, {"ok": True, "languages": list(WORKBENCH_LANGS)})
        else:
            self._send(404, {"error": "not found"})

    def do_POST(self):
        if self.path in ("/api/compile", "/api/simulate"):
            length = int(self.headers.get("Content-Length") or 0)
            if length > MAX_CODE_BYTES * 2:
                return self._send(413, {"error": f"That code is too large. The limit is {MAX_CODE_BYTES // 1000} KB."})
            try:
                req = json.loads(self.rfile.read(length) or b"{}")
                self._send(200, compile_code(req) if self.path == "/api/compile" else simulate(req))
            except subprocess.TimeoutExpired:
                self._send(504, {"error": f"The compiler took longer than {COMPILE_TIMEOUT} s and was stopped."})
            except (ValueError, RuntimeError) as e:
                self._send(400, {"error": str(e)})
            except Exception as e:   # keep the server alive and tell the page what happened
                self._send(500, {"error": f"The compiler failed: {e}"})
            return
        self._send(404, {"error": "not found"})

    def log_message(self, fmt, *args):   # one short line per request instead of the default
        sys.stderr.write(f"  {self.command} {self.path} -> {args[1] if len(args) > 1 else ''}\n")


def main(args):
    port = int(args[args.index("--port") + 1]) if "--port" in args else 8000
    subprocess.run(["make", "-s", "build/rcc"], cwd=ROOT, check=True)

    for p in range(port, port + 20):           # take the first free port
        try:
            server = ThreadingHTTPServer(("127.0.0.1", p), Handler)
            break
        except OSError:
            continue
    else:
        sys.exit(f"No free port between {port} and {port + 19}.")

    url = f"http://127.0.0.1:{server.server_address[1]}/"
    print(f"Compiler workbench running at {url}  (Ctrl+C to stop)")
    if "--no-open" not in args:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main(sys.argv[1:])
