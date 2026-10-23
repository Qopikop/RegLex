"""Runs the visualizer as a local page where you can type, paste or open any code.

The page sends the code here; this server detects the language, picks the
matching fixed rules (rules/c.rules, rules/python.rules, or the built-in
general rules), runs the real C lexer on it via trace_json, and sends the
trace back for the page to draw. Nothing leaves this machine: the server
listens on 127.0.0.1 only.

Usage, from the project root:
    python3 visualize/serve.py            # opens http://127.0.0.1:8000/ in your browser
    python3 visualize/serve.py --port 9000 --no-open
"""
import json
import pathlib
import sys
import tempfile
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import build   # noqa: E402  (same directory)
import detect  # noqa: E402

MAX_CODE_BYTES = 200_000
LANGUAGE_CHOICES = ["C", "Python", "Java", "JavaScript", "Other"]
SUFFIX = {"C": ".c", "Python": ".py", "Java": ".java", "JavaScript": ".js", "Other": ".txt"}

PAGE = b""       # built once at startup: the demo programs plus the code box
NFA_PAGE = b""   # the token rules and their NFAs


def lex(code, language="auto", filename=None):
    if language in (None, "", "auto"):
        found = detect.detect(code, filename)
    elif language in LANGUAGE_CHOICES:
        found = {"language": language, "rules": detect.rules_for(language), "reason": "chosen by you"}
    else:
        raise ValueError(f"unknown language {language!r}")

    name = pathlib.Path(filename).name if filename else "your code" + SUFFIX[found["language"]]
    with tempfile.TemporaryDirectory() as tmp:
        path = pathlib.Path(tmp) / ("input" + SUFFIX[found["language"]])
        path.write_text(code, encoding="utf-8")
        rules_label = (pathlib.Path(found["rules"]).stem.capitalize() + " rules") if found["rules"] else "general rules"
        run = build.trace(path, found["rules"], label=f"{found['language']} · {rules_label}")
    run["trace"]["sourceName"] = name          # show the user's name, not the temp path
    run["detected"] = {"language": found["language"], "rules": found["rules"] or "built-in general rules",
                       "reason": found["reason"]}
    return run


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
        if path in ("/", "/index.html", "/lexer_visualizer.html"):
            self._send(200, PAGE, "text/html; charset=utf-8")
        elif path == "/nfa.html":
            self._send(200, NFA_PAGE, "text/html; charset=utf-8")
        elif path == "/api/health":
            self._send(200, {"ok": True, "languages": LANGUAGE_CHOICES})
        else:
            self._send(404, {"error": "not found"})

    def do_POST(self):
        if self.path != "/api/lex":
            return self._send(404, {"error": "not found"})
        length = int(self.headers.get("Content-Length") or 0)
        if length > MAX_CODE_BYTES * 2:
            return self._send(413, {"error": f"That code is too large. The limit is {MAX_CODE_BYTES // 1000} KB."})
        try:
            req = json.loads(self.rfile.read(length) or b"{}")
            code = req.get("code", "")
            if not isinstance(code, str) or not code.strip():
                return self._send(400, {"error": "Type, paste or open some code first."})
            if len(code.encode("utf-8")) > MAX_CODE_BYTES:
                return self._send(413, {"error": f"That code is too large. The limit is {MAX_CODE_BYTES // 1000} KB."})
            self._send(200, lex(code, req.get("language"), req.get("filename")))
        except (ValueError, RuntimeError) as e:
            self._send(400, {"error": str(e)})
        except Exception as e:   # keep the server alive and tell the page what happened
            self._send(500, {"error": f"The lexer failed: {e}"})

    def log_message(self, fmt, *args):   # one short line per request instead of the default
        sys.stderr.write(f"  {self.command} {self.path} -> {args[1] if len(args) > 1 else ''}\n")


def main(args):
    global PAGE, NFA_PAGE
    port = int(args[args.index("--port") + 1]) if "--port" in args else 8000
    build.compile_tracer()
    runs = build.demo_runs()
    PAGE = build.page_html(runs).encode("utf-8")
    NFA_PAGE = build.nfa_page_html(runs).encode("utf-8")

    for p in range(port, port + 20):           # take the first free port
        try:
            server = ThreadingHTTPServer(("127.0.0.1", p), Handler)
            break
        except OSError:
            continue
    else:
        sys.exit(f"No free port between {port} and {port + 19}.")

    url = f"http://127.0.0.1:{server.server_address[1]}/"
    print(f"Lexer visualizer running at {url}  (Ctrl+C to stop)")
    if "--no-open" not in args:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main(sys.argv[1:])
