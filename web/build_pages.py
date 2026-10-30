"""Builds the files GitHub Pages needs to show the workbench with no server:

    web/examples.json        what GET /api/examples returns
    web/results/<n>.json     the real compiler output (POST /api/compile) for each example,
                             made with build/rcc exactly as visualize/serve.py makes it
    web/results/index.json   sha256(code + NUL + rules) -> result file, so the page can find them

Run after changing the compiler or the examples:  make && python3 web/build_pages.py
"""
import hashlib
import json
import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "visualize"))
import serve  # noqa: E402

WEB = ROOT / "web"


def key(code, rules=""):
    return hashlib.sha256((code + "\0" + (rules or "")).encode("utf-8")).hexdigest()


def main():
    ex = serve.examples()
    (WEB / "examples.json").write_text(json.dumps(ex, ensure_ascii=False), encoding="utf-8")
    out = WEB / "results"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir()
    index = {}
    jobs = [{"code": x["code"], "language": x["language"], "filename": x["file"].split("/")[-1]} for x in ex["examples"]]
    jobs += [{"code": x["code"], "language": "C", "filename": x["file"].split("/")[-1], "rules": x["rulesText"]} for x in ex["lexical"]]
    for n, req in enumerate(jobs):
        data = serve.compile_code(req)
        (out / f"{n}.json").write_text(json.dumps(data, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
        index[key(req["code"], req.get("rules"))] = f"{n}.json"
        print(f"  {req['filename']:<24} {data['diagnosticCounts']}")
    (out / "index.json").write_text(json.dumps(index, indent=0), encoding="utf-8")
    size = sum(f.stat().st_size for f in out.iterdir()) / 1e6
    print(f"{len(jobs)} results, {size:.1f} MB")


if __name__ == "__main__":
    main()
