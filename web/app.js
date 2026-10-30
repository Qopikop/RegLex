/* RegLex Compiler Workbench -- page logic.
   Everything shown comes from build/rcc's JSON: POST /api/compile via visualize/serve.py, or, with no
   server (GitHub Pages), the same compiler built to WebAssembly and run in the browser (browser-compiler.js).
   Sections: helpers · status & staleness · editor · menus & examples · pipeline · views · compile · export · boot */
"use strict";
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const esc = s => String(s ?? "").replace(/[&<>"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
const store = { get(k, d) { try { const v = localStorage.getItem("rlw." + k); return v === null ? d : JSON.parse(v); } catch { return d; } },
                set(k, v) { try { localStorage.setItem("rlw." + k, JSON.stringify(v)); } catch {} } };
const plural = (n, one, many = one + "s") => `${n} ${n === 1 ? one : many}`;

const state = { data: null, examples: null, view: store.get("view", "tokens"), auto: { rule: 0, mode: "min" }, passes: null,
                fn: "*", focusLine: 0, sim: null, compiling: false, compiled: null, failed: null, menu: null, exampleLang: null };

/* ------------------------------------------------------------------ theme */
const root = document.documentElement;
const savedTheme = store.get("theme", null);
if (savedTheme) root.dataset.theme = savedTheme;
$("#themeBtn").onclick = () => {
  const dark = root.dataset.theme ? root.dataset.theme === "dark" : matchMedia("(prefers-color-scheme: dark)").matches;
  root.dataset.theme = dark ? "light" : "dark";
  store.set("theme", root.dataset.theme);
};

function toast(msg, kind = "") {
  const t = $("#toast");
  t.innerHTML = (kind ? `<span class="dot ${kind}"></span>` : "") + esc(msg);
  t.classList.add("show");
  clearTimeout(toast.t);
  toast.t = setTimeout(() => t.classList.remove("show"), 2600);
}

/* ------------------------------------------------------------------ status & staleness */
/* The inputs that decide a compile. Results are stale when any of them differs from the last compile. */
function currentInputs() {
  return { code: code.value, lang: $("#lang").value, rules: $("#customRules").checked ? $("#rulesText").value : null, passes: JSON.stringify(state.passes) };
}
function staleReason() {
  if (!state.data || !state.compiled) return null;
  const now = currentInputs(), was = state.compiled;
  if (now.code !== was.code) return "source";
  if (now.lang !== was.lang || now.rules !== was.rules || now.passes !== was.passes) return "settings";
  return null;
}
/* One summary of the last compile, derived from the compiler's own JSON. */
function compileSummary() {
  if (state.compiling) return { kind: "run", label: "Compiling…" };
  if (state.failed?.startsWith("This online demo")) return { kind: "neutral", label: "Online demo: examples only", detail: "run ./run.sh to compile your own code" };
  if (state.failed) return { kind: "error", label: "Compile failed", detail: state.failed };
  const d = state.data;
  if (!d) return { kind: "", label: "Ready", detail: "press Compile" };
  const stale = staleReason();
  if (stale) return { kind: "stale", label: (stale === "source" ? "Source" : "Settings") + " changed — compile again", detail: "results below are from the previous compile" };
  const errs = d.diagnosticCounts.errors;
  if (errs) return { kind: "error", label: "Errors found", detail: plural(errs, "error") };
  if (d.source.customRules || !d.tac) return { kind: "neutral", label: "Lexical analysis only", detail: d.source.customRules ? "custom rules" : "" };
  if (d.run?.equivalent === false) return { kind: "error", label: "Errors found", detail: "the optimized run behaved differently" };
  if (!d.tac.complete || d.tac.opaque) return { kind: "warn", label: "Partial translation", detail: plural(d.tac.opaque, "construct") + " not translated" };
  return { kind: "ok", label: "Complete", detail: d.run?.equivalent ? "original and optimized runs matched" : "" };
}
function renderStatus() {
  const s = compileSummary(), el = $("#status");
  el.dataset.kind = s.kind;
  el.innerHTML = `<span class="dot ${s.kind}"></span><b>${esc(s.label)}</b>${s.detail ? `<span class="detail">${esc(s.detail)}</span>` : ""}`;
  el.title = state.data && !state.compiling ? state.data.stages.map((x, i) => `${STAGES[i]?.[1] || x.name}: ${x.summary || x.status}`).join("\n") : "";
}
let lastStale = null;
function refreshStale() {
  const stale = !!staleReason();
  $("#results").classList.toggle("stale", stale);
  $("#pipeline").classList.toggle("stale", stale);
  if (stale !== lastStale) { lastStale = stale; updateExportMenu(); }
  renderStatus();
}

/* ------------------------------------------------------------------ editor */
const code = $("#code"), hl = $("#hl"), gutter = $("#gutter");
const KW = {
  C: "auto break case char const continue default do double else enum extern float for goto if inline int long register restrict return short signed sizeof static struct switch typedef union unsigned void volatile while _Bool bool true false NULL",
  Java: "abstract assert boolean break byte case catch char class const continue default do double else enum extends final finally float for if implements import instanceof int interface long native new package private protected public return short static super switch synchronized this throw throws transient try void volatile while true false null var record",
  Python: "False None True and as assert async await break class continue def del elif else except finally for from global if import in is lambda nonlocal not or pass raise return try while with yield self print range len",
};
function langOf() {
  const l = $("#lang").value;
  if (l !== "auto") return l;
  if (state.data && !state.data.source.customRules) return state.data.source.language;
  const s = code.value;
  if (/^\s*(def|class)\s+\w+.*:\s*$/m.test(s) || /^\s*import \w+\s*$/m.test(s) && !/;/.test(s)) return "Python";
  if (/\bpublic\s+(static\s+)?(class|void)\b|System\.out/.test(s)) return "Java";
  return "C";
}
function highlight(src, lang) {
  const kw = new Set(KW[lang].split(" "));
  const re = lang === "Python"
    ? /(#[^\n]*)|([rRbBfFuU]{0,2}(?:"""[\s\S]*?(?:"""|$)|'''[\s\S]*?(?:'''|$)|"(?:[^"\\\n]|\\.)*"?|'(?:[^'\\\n]|\\.)*'?))|(\b\d[\d_]*\.?\d*(?:[eE][+-]?\d+)?j?\b|\b0[xXoObB][\da-fA-F_]+\b)|([A-Za-z_]\w*)(\s*\()?|([-+*/%=<>!&|^~@:]+)/g
    : /(\/\/[^\n]*|\/\*[\s\S]*?(?:\*\/|$))|("(?:[^"\\\n]|\\.)*"?|'(?:[^'\\\n]|\\.)*'?)|(\b0[xX][\da-fA-F]+[uUlL]*\b|\b\d+\.?\d*(?:[eE][+-]?\d+)?[fFlLuUdD]*\b)|([A-Za-z_$][\w$]*)(\s*\()?|(^[ \t]*#[^\n]*|[-+*/%=<>!&|^~?]+)/gm;
  let out = "", last = 0, m;
  while ((m = re.exec(src))) {
    out += esc(src.slice(last, m.index));
    if (m[1]) out += `<span class="c">${esc(m[1])}</span>`;
    else if (m[2]) out += `<span class="s">${esc(m[2])}</span>`;
    else if (m[3]) out += `<span class="n">${esc(m[3])}</span>`;
    else if (m[4]) out += kw.has(m[4]) ? `<span class="k">${esc(m[4])}</span>${esc(m[5] || "")}` : m[5] ? `<span class="f">${esc(m[4])}</span>${esc(m[5])}` : esc(m[4]);
    else if (m[6]) out += m[6].trim().startsWith("#") ? `<span class="d">${esc(m[6])}</span>` : `<span class="o">${esc(m[6])}</span>`;
    last = re.lastIndex;
  }
  return out + esc(src.slice(last)) + "\n";
}
let renderQueued = false;
function renderEditor() {
  if (renderQueued) return;
  renderQueued = true;
  requestAnimationFrame(() => {
    renderQueued = false;
    const src = code.value, lang = langOf();
    hl.innerHTML = searchMark(highlight(src, lang));
    const n = src.split("\n").length;
    const marks = lineMarks();
    let g = "";
    for (let i = 1; i <= n; i++) g += `<div class="${marks[i] || ""}">${i}</div>`;
    gutter.innerHTML = g;
    $("#docInfo").textContent = `${n} lines · ${src.length.toLocaleString()} chars`;
    $("#langInfo").textContent = lang + ($("#lang").value === "auto" ? " (auto)" : "");
    syncScroll();
    drawLineMarks();
    refreshStale();
    store.set("code", src.length < 400000 ? src : "");
  });
}
function lineMarks() {
  const m = {};
  for (const d of state.data?.diagnostics || []) {
    if (!d.line) continue;
    if (d.severity === "error") m[d.line] = "err";
    else if (d.severity === "warning" && !m[d.line]) m[d.line] = "warn";
  }
  return m;
}
function drawLineMarks() {
  const marks = lineMarks();
  let html = "";
  for (const [ln, k] of Object.entries(marks)) html += `<div class="hl-line ${k}" style="top:${12 + (ln - 1) * 20 - code.scrollTop}px"></div>`;
  if (state.focusLine) html += `<div class="hl-line focus" style="top:${12 + (state.focusLine - 1) * 20 - code.scrollTop}px"></div>`;
  $("#lineMarks").innerHTML = html;
}
function syncScroll() { hl.scrollTop = code.scrollTop; hl.scrollLeft = code.scrollLeft; gutter.scrollTop = code.scrollTop; drawLineMarks(); }
code.addEventListener("scroll", syncScroll);
code.addEventListener("input", () => { renderEditor(); });
function updateCursor() {
  const pre = code.value.slice(0, code.selectionStart);
  const line = pre.split("\n").length, col = pre.length - pre.lastIndexOf("\n");
  $("#cursorPos").textContent = `Ln ${line}, Col ${col}`;
  $$(".gutter div.cur").forEach(d => d.classList.remove("cur"));
  gutter.children[line - 1]?.classList.add("cur");
}
["keyup", "click", "select"].forEach(e => code.addEventListener(e, updateCursor));
code.addEventListener("keydown", e => {
  const s = code.selectionStart, en = code.selectionEnd, v = code.value;
  if ((e.metaKey || e.ctrlKey) && e.key === "Enter") { e.preventDefault(); compile(); return; }
  if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "f") { e.preventDefault(); openSearch(); return; }
  if (e.key === "Tab") {
    e.preventDefault();
    if (e.shiftKey) {
      const ls = v.lastIndexOf("\n", s - 1) + 1;
      const k = v.slice(ls, ls + 4).match(/^ {0,4}/)[0].length;
      code.setRangeText("", ls, ls + k, "preserve");
      code.selectionStart = code.selectionEnd = Math.max(ls, s - k);
    } else code.setRangeText("    ", s, en, "end");
    renderEditor();
  } else if (e.key === "Enter" && !e.shiftKey) {
    e.preventDefault();
    const ls = v.lastIndexOf("\n", s - 1) + 1;
    let ind = v.slice(ls, s).match(/^[ \t]*/)[0];
    const before = v.slice(ls, s).trimEnd();
    if (/[{:(\[]$/.test(before)) ind += "    ";
    const close = v[en] === "}" && before.endsWith("{");
    code.setRangeText("\n" + ind + (close ? "\n" + ind.slice(4) : ""), s, en, "end");
    if (close) code.selectionStart = code.selectionEnd = s + 1 + ind.length;
    renderEditor();
  } else if ("([{".includes(e.key) && s === en && /^(\s|$|[)\]};,])/.test(v.slice(s, s + 1) || " ")) {
    e.preventDefault();
    const pair = { "(": ")", "[": "]", "{": "}" }[e.key];
    code.setRangeText(e.key + pair, s, en, "start");
    code.selectionStart = code.selectionEnd = s + 1;
    renderEditor();
  } else if (")]}".includes(e.key) && v[s] === e.key && s === en) {
    e.preventDefault();
    code.selectionStart = code.selectionEnd = s + 1;
  }
});
function gotoLine(line, col = 1) {
  if (!line) return;
  const lines = code.value.split("\n");
  let pos = 0;
  for (let i = 0; i < line - 1 && i < lines.length; i++) pos += lines[i].length + 1;
  pos += Math.max(0, col - 1);
  code.focus();
  code.setSelectionRange(pos, pos);
  code.scrollTop = Math.max(0, (line - 6) * 20);
  state.focusLine = line;
  drawLineMarks();
  updateCursor();
}

/* search */
let searchTerm = "", searchIdx = -1;
function searchMark(html) {
  if (!searchTerm) return html;
  // mark matches in text nodes only
  const t = esc(searchTerm).replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  return html.replace(/(<[^>]+>)|([^<]+)/g, (m, tag, text) => tag || text.replace(new RegExp(t, "gi"), x => `<span class="search-hit">${x}</span>`));
}
function openSearch() { $("#searchbox").classList.add("open"); $("#searchInput").focus(); $("#searchInput").select(); }
$("#findBtn").onclick = openSearch;
$("#searchInput").oninput = e => { searchTerm = e.target.value; searchIdx = -1; const n = searchTerm ? code.value.toLowerCase().split(searchTerm.toLowerCase()).length - 1 : 0; $("#searchCount").textContent = n; renderEditor(); };
function findNext() {
  if (!searchTerm) return;
  const v = code.value.toLowerCase(), t = searchTerm.toLowerCase();
  let i = v.indexOf(t, searchIdx + 1);
  if (i < 0) i = v.indexOf(t);
  if (i < 0) return;
  searchIdx = i;
  const line = v.slice(0, i).split("\n").length;
  code.focus(); code.setSelectionRange(i, i + t.length);
  code.scrollTop = Math.max(0, (line - 6) * 20);
}
$("#searchNext").onclick = findNext;
$("#searchInput").addEventListener("keydown", e => { if (e.key === "Enter") { e.preventDefault(); findNext(); } if (e.key === "Escape") closeSearch(); });
function closeSearch() { $("#searchbox").classList.remove("open"); searchTerm = ""; renderEditor(); code.focus(); }
$("#searchClose").onclick = closeSearch;

/* toolbar */
$("#copyBtn").onclick = async () => { try { await navigator.clipboard.writeText(code.value); toast("Source copied"); } catch { toast("Copy is blocked in this browser", "warn"); } };
$("#clearBtn").onclick = () => { code.value = ""; clearResults(); };
$("#file").onchange = async e => {
  const f = e.target.files[0];
  if (!f) return;
  if (f.size > 1_000_000) { toast("That file is larger than 1 MB", "error"); return; }
  code.value = await f.text();
  state.filename = f.name;
  const ext = f.name.split(".").pop().toLowerCase();
  const l = { c: "C", h: "C", java: "Java", py: "Python" }[ext];
  if (l) { $("#lang").value = l; store.set("lang", l); syncRulesText(); }
  clearResults();
  e.target.value = "";
};
/* Switching language shows that language's sample program. */
$("#lang").onchange = () => {
  const l = $("#lang").value;
  store.set("lang", l);
  const ex = state.examples?.examples.find(x => x.file === SAMPLE[l]);
  if (ex) { loadExample(ex); toast(`Loaded the ${l} sample: ${ex.title}`); }
  syncRulesText();
  renderEditor();
};
$("#rulesBtn").onclick = () => toggleRules();
function toggleRules(open) {
  const d = $("#rulesDrawer"), want = open ?? !d.classList.contains("open");
  d.classList.toggle("open", want);
  $("#rulesBtn").setAttribute("aria-expanded", want);
  syncRulesText();
}
$("#rulesText").oninput = () => { $("#customRules").checked = true; store.set("rules", $("#rulesText").value); refreshStale(); };
$("#customRules").onchange = () => { if (!$("#customRules").checked) syncRulesText(true); refreshStale(); };
function syncRulesText(force) {
  if (!state.examples || ($("#customRules").checked && !force)) return;
  $("#rulesText").value = state.examples.defaultRules[langOf()] || "";
}
$("#compileBtn").onclick = () => compile();
document.addEventListener("keydown", e => { if ((e.metaKey || e.ctrlKey) && e.key === "Enter" && document.activeElement !== code) { e.preventDefault(); compile(); } });

/* splitter */
(() => {
  const sp = $("#splitter"), ws = $("#workspace");
  let drag = false;
  const saved = store.get("split", null);
  if (saved) ws.style.setProperty("--split", saved + "%");
  sp.addEventListener("pointerdown", e => { drag = true; sp.classList.add("drag"); sp.setPointerCapture(e.pointerId); });
  sp.addEventListener("pointermove", e => {
    if (!drag) return;
    const r = ws.getBoundingClientRect();
    const pct = Math.min(70, Math.max(25, (e.clientX - r.left) / r.width * 100));
    ws.style.setProperty("--split", pct + "%");
    store.set("split", pct);
  });
  sp.addEventListener("pointerup", () => { drag = false; sp.classList.remove("drag"); });
  sp.addEventListener("keydown", e => {
    const cur = parseFloat(getComputedStyle(ws).getPropertyValue("--split")) || 44;
    if (e.key === "ArrowLeft" || e.key === "ArrowRight") { const v = Math.min(70, Math.max(25, cur + (e.key === "ArrowLeft" ? -2 : 2))); ws.style.setProperty("--split", v + "%"); store.set("split", v); }
  });
})();

/* console */
$("#consoleHead").onclick = () => toggleConsole();
$("#consoleHead").onkeydown = e => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); toggleConsole(); } };
function toggleConsole(open) {
  const want = open ?? $("#consoleHead").getAttribute("aria-expanded") !== "true";
  root.style.setProperty("--console-h", want ? "200px" : "36px");
  $("#consoleHead").setAttribute("aria-expanded", want);
}

/* ------------------------------------------------------------------ menus & examples */
/* One open panel at a time; Escape or a click outside closes it. */
function openPanel(btn, panel) {
  closeMenus();
  panel.classList.add("open");
  panel.style.right = "";                   // keep the panel inside the window
  const r = panel.getBoundingClientRect();
  if (r.left < 8) panel.style.right = `${r.left - 8}px`;
  else if (r.right > innerWidth - 8) panel.style.right = `${r.right - innerWidth + 8}px`;
  btn.setAttribute("aria-expanded", "true");
  state.menu = { btn, panel };
  menuItems(panel)[0]?.focus();
}
function closeMenus(returnFocus) {
  $$(".menu-list.open").forEach(p => p.classList.remove("open"));
  $$(".menu > [aria-haspopup]").forEach(b => b.setAttribute("aria-expanded", "false"));
  if (returnFocus && state.menu) state.menu.btn.focus();
  state.menu = null;
}
const menuItems = panel => $$(".menu-item:not(:disabled), .example, .seg button", panel);
function togglePanel(btn, panel) { if (panel.classList.contains("open")) closeMenus(); else openPanel(btn, panel); }
$("#exportBtn").onclick = () => togglePanel($("#exportBtn"), $("#exportList"));
$("#openBtn").onclick = () => togglePanel($("#openBtn"), $("#openList"));
$("#openFile").onclick = () => { closeMenus(); $("#file").click(); };
$("#openExample").onclick = () => { renderExamplePicker(); openPanel($("#openBtn"), $("#examplePicker")); $("#exampleList .example")?.focus(); };
document.addEventListener("click", e => {
  // composedPath, not closest: a click can re-render (and detach) the element that was clicked
  if (state.menu && !e.composedPath().some(n => n.classList?.contains("menu"))) closeMenus();
  const b = e.target.closest("[data-explain]");
  if (b) { const ex = $("#ex-" + b.dataset.explain); ex?.classList.toggle("open"); b.setAttribute("aria-expanded", !!ex?.classList.contains("open")); }
});
document.addEventListener("keydown", e => {
  if (!state.menu) {
    // graph nodes are focusable: Enter or Space acts like a click
    if ((e.key === "Enter" || e.key === " ") && e.target.closest?.(".node")) { e.preventDefault(); e.target.closest(".node").dispatchEvent(new MouseEvent("click", { bubbles: true })); }
    return;
  }
  if (e.key === "Escape") { e.preventDefault(); e.stopImmediatePropagation(); closeMenus(true); return; }
  if (e.key === "ArrowDown" || e.key === "ArrowUp") {
    const items = menuItems(state.menu.panel), i = items.indexOf(document.activeElement);
    if (!items.length) return;
    e.preventDefault();
    items[(i + (e.key === "ArrowDown" ? 1 : -1) + items.length) % items.length].focus();
  }
  if (e.key === "Tab" && !state.menu.panel.contains(document.activeElement)) closeMenus();
}, true);

const LEVELS = ["Basic", "Intermediate", "Advanced", "Partial", "Errors"];
const LEVEL_NAME = { Partial: "Partial support", Errors: "Errors and recovery" };
function renderExamplePicker() {
  const ex = state.examples, list = $("#exampleList");
  if (!ex) { $("#exampleLangs").innerHTML = ""; list.innerHTML = `<p class="note" style="padding:8px 9px">The examples could not be loaded. Locally, start the server with <span class="mono">./run.sh</span>.</p>`; return; }
  const langs = ["C", "Java", "Python", ...(ex.lexical?.length ? ["Custom rules"] : [])];
  if (!langs.includes(state.exampleLang)) state.exampleLang = langs.includes(langOf()) ? langOf() : "C";
  $("#exampleLangs").innerHTML = langs.map(l => `<button data-exlang="${esc(l)}" class="${l === state.exampleLang ? "sel" : ""}" aria-pressed="${l === state.exampleLang}">${esc(l === "Custom rules" ? "Rules" : l)}</button>`).join("");
  $$("#exampleLangs button").forEach(b => b.onclick = () => { state.exampleLang = b.dataset.exlang; renderExamplePicker(); $(`#exampleLangs [data-exlang="${b.dataset.exlang}"]`).focus(); });
  const items = state.exampleLang === "Custom rules"
    ? ex.lexical.map((x, i) => ({ ...x, level: "Custom rules · lexical analysis only", key: "lex:" + i }))
    : ex.examples.filter(x => x.language === state.exampleLang).map(x => ({ ...x, key: x.file }));
  const levels = [...new Set(items.map(x => x.level))].sort((a, b) => (LEVELS.indexOf(a) + 99) % 99 - (LEVELS.indexOf(b) + 99) % 99);
  list.innerHTML = levels.map(l => `<div class="examples-level">${esc(LEVEL_NAME[l] || l)}</div>` +
    items.filter(x => x.level === l).map(x => `<button class="example" data-key="${esc(x.key)}" title="${esc(x.file)}"><span class="t">${esc(x.title)}</span><span class="a">${esc(x.about)}</span></button>`).join("")).join("");
  $$("#exampleList .example").forEach(b => b.onclick = () => {
    const x = items.find(i => i.key === b.dataset.key);
    closeMenus(true);
    loadExample(x);
    toast(`Loaded ${x.title}`);
  });
}
/* Explicit user action only: replaces the source with an example. */
function loadExample(x) {
  code.value = x.code;
  state.filename = x.file.split("/").pop();
  if (x.rulesText != null) {
    $("#rulesText").value = x.rulesText;
    $("#customRules").checked = true;
    store.set("rules", x.rulesText);
    toggleRules(true);
  } else {
    $("#lang").value = x.language;
    store.set("lang", x.language);
    $("#customRules").checked = false;
    syncRulesText(true);
  }
  clearResults();
}
const SAMPLE = { C: "c/05_optimizations.c", Java: "java/Optimizations.java", Python: "python/optimizations.py" };

/* ------------------------------------------------------------------ pipeline */
const STAGES = [
  ["rules", "Rules & automata", "automata"], ["lex", "Lexical analysis", "tokens"], ["parse", "Syntax analysis", "syntax"],
  ["sema", "Semantic analysis", "syntax"], ["tac", "Intermediate code", "code"], ["opt", "Optimization", "code"], ["run", "Run & compare", "run"],
];
function renderPipeline(running) {
  const byId = Object.fromEntries((state.data?.stages || []).map(s => [s.id, s]));
  $("#pipeline").innerHTML = STAGES.map(([id, name, view], i) => {
    const s = byId[id];
    const st = running ? "running" : s ? s.status : "pending";
    const icon = { ok: "✓", warn: "!", error: "✕", skipped: "–", running: "", pending: i + 1 }[st] ?? "";
    const sum = running ? "running…" : s ? (s.summary || (s.ms ? s.ms.toFixed(1) + " ms" : s.status)) : "not run yet";
    return (i ? `<span class="arrow" aria-hidden="true">›</span>` : "") +
      `<button class="stage ${state.view === view ? "sel" : ""}" data-status="${st}" data-view="${view}" title="${esc(name)}: ${esc(sum)}">
        <span class="st-icon" aria-hidden="true">${icon}</span><span class="st-text"><div class="st-name">${esc(name)}</div><div class="st-sum">${esc(sum)}</div></span></button>`;
  }).join("");
  $$("#pipeline .stage").forEach(b => b.onclick = () => showView(b.dataset.view));
}

/* ------------------------------------------------------------------ views */
function showView(v) {
  state.view = v;
  store.set("view", v);
  $$("#tabs .tab").forEach(t => { t.classList.toggle("sel", t.dataset.view === v); t.setAttribute("aria-selected", t.dataset.view === v); t.tabIndex = t.dataset.view === v ? 0 : -1; });
  $$(".view").forEach(el => el.classList.toggle("sel", el.id === "v-" + v));
  $$("#pipeline .stage").forEach(b => b.classList.toggle("sel", b.dataset.view === v));
  if (v === "automata") renderAutomata();
  if (v === "syntax") renderSyntax();
}
$$("#tabs .tab").forEach(t => t.onclick = () => showView(t.dataset.view));
$("#tabs").addEventListener("keydown", e => {
  if (e.key !== "ArrowLeft" && e.key !== "ArrowRight") return;
  const tabs = $$("#tabs .tab"), i = tabs.findIndex(t => t.classList.contains("sel"));
  const next = tabs[(i + (e.key === "ArrowRight" ? 1 : -1) + tabs.length) % tabs.length];
  showView(next.dataset.view); next.focus();
});

const EXPLAIN = {
  tokens: `<p><b>Lexical analysis</b> splits the source into tokens. Every rule is a regular expression; at each position the scanner takes the <b>longest</b> match (maximal munch) and breaks ties by <b>priority</b>, so <span class="mono">if</span> is a keyword but <span class="mono">iffy</span> is an identifier.</p><p>The scanner runs on the <b>minimized DFA</b>. The NFA and the unminimized DFA scan the same text too, and the page checks that all three give identical tokens for this input.</p>`,
  automata: `<p><b>Thompson's construction</b> turns each regex into an NFA with ε-moves. <b>Subset construction</b> makes a DFA whose states are sets of NFA states, the <b>ε-closures</b> reachable after each input symbol. <b>Minimization</b> (partition refinement) merges DFA states that no input can tell apart, while never merging states that accept different token types.</p><p>Bytes that no state distinguishes share one input symbol, so the tables stay small.</p>`,
  syntax: `<p>The <b>parser</b> (recursive descent) checks the token sequence against the supported grammar for C, Java or Python and builds an <b>abstract syntax tree</b>: the program's structure without punctuation. <b>Semantic analysis</b> walks it with a scoped symbol table and reports undeclared or duplicate names, type mismatches and wrong argument counts.</p>`,
  code: `<p><b>Three-address code</b> (TAC) has at most one operator per instruction (<span class="mono">t1 = b * 2</span>), with temporaries, labels and jumps. It is easy to analyse, so the optimizer works on it, using a <b>control-flow graph</b> and data-flow analyses (reaching constants, available copies and expressions, liveness, dominators for loops).</p><p>Each change is only made when it is safe under the language's rules. C signed overflow is undefined, Java wraps around, Python integers are unbounded, and a value of unknown type may overload its operators.</p>`,
  run: `<p>The original and the optimized TAC are both executed by a sandboxed interpreter that follows each language's arithmetic and printing rules. Matching output on this run is evidence, not proof, that the optimizer preserved the program's behaviour. The test suite also compares the TAC against the real gcc, javac and python3.</p>`,
};
function head(title, key, extra = "") {
  return `<div class="view-head"><h2>${title}</h2><button class="info" data-explain="${key}" title="What is this?" aria-label="Explain ${esc(title)}" aria-expanded="false" aria-controls="ex-${key}">i</button><span class="spacer"></span>${extra}</div><div class="explain" id="ex-${key}">${EXPLAIN[key]}</div>`;
}
function empty(msg, sub) { return `<div class="empty"><div><b>${msg}</b>${sub ? `<span>${sub}</span>` : ""}</div></div>`; }
function callout(kind, icon, title, body = "") { return `<div class="callout ${kind}"><span class="ic" aria-hidden="true">${icon}</span><div class="body"><b>${title}</b>${body}</div></div>`; }
const lexOnlyNote = d => callout("neutral", "i", "Lexical analysis only",
  d.source.customRules ? "These results come from custom lexical rules. Custom rules are not tied to a grammar, so parsing, intermediate code and execution are skipped. Turn off custom rules to compile with the built-in C, Java or Python rules." : "Only the lexer ran for this source.");
const fact = (v, label, cls = "") => `<span class="${cls}"><b>${v}</b>${label}</span>`;
const fmt = n => (n ?? 0).toLocaleString();
const ms = n => n == null ? "–" : n < 1 ? n.toFixed(3) + " ms" : n < 100 ? n.toFixed(2) + " ms" : n.toFixed(0) + " ms";

function renderAll() {
  renderPipeline(false);
  renderTokens(); renderCode(); renderRun(); renderDiagnostics();
  if (state.view === "automata") renderAutomata(); else $("#v-automata").dataset.stale = "1";
  if (state.view === "syntax") renderSyntax(); else $("#v-syntax").dataset.stale = "1";
  const d = state.data;
  $("#b-tokens").textContent = d ? fmt(d.tokens.count) : "";
  $("#b-automata").textContent = d?.rules?.combined ? d.rules.combined.minStates + " st" : "";
  $("#b-syntax").textContent = d?.astNodes ? fmt(d.astNodes) : "";
  $("#b-code").textContent = d?.tac ? `${d.tac.originalCount}→${d.tac.optimizedCount}` : "";
  $("#b-run").textContent = d?.run ? (d.run.equivalent === true ? "✓" : d.run.equivalent === false ? "✕" : "n/a") : "";
  updateExportMenu();
  refreshStale();
}

/* ---- tokens ---- */
const TOK_COLOR = { KEYWORD: "var(--t-kw)", IDENT: "var(--t-id)", NUMBER: "var(--t-num)", STRING: "var(--t-str)", CHAR: "var(--t-str)", OP: "var(--t-op)", PUNCT: "var(--t-punct)", DIRECTIVE: "var(--t-dir)", NEWLINE: "var(--faint)", INDENT: "var(--accent)", DEDENT: "var(--accent)" };
let tokFilter = "", tokRule = "", tokLimit = 600;
function renderTokens() {
  const v = $("#v-tokens"), d = state.data;
  if (!d) { v.innerHTML = empty("Nothing compiled yet", "Type or open some code, then press Compile."); return; }
  const eq = d.lexEquivalence;
  const counts = {};
  for (const t of d.tokens.list) counts[t[0]] = (counts[t[0]] || 0) + 1;
  const maxMs = Math.max(eq.nfa.ms, eq.dfa.ms, eq.min.ms) || 1;
  const speedup = eq.dfa.ms ? (eq.nfa.ms / eq.dfa.ms).toFixed(1) : null;
  const list = d.tokens.list.filter(t => (!tokRule || t[0] === tokRule) && (!tokFilter || t[4].toLowerCase().includes(tokFilter) || t[0].toLowerCase().includes(tokFilter)));
  v.innerHTML = head("Lexical analysis", "tokens", `<span class="mono muted" style="font-size:12px">${esc(d.rules?.name?.split("/").pop() || "")}</span>`) +
    (d.source.customRules ? lexOnlyNote(d) : "") + `
    <div class="facts">
      ${fact(fmt(d.tokens.count), "tokens")}${fact(fmt(d.metrics.lines), "lines")}${fact(d.tokens.lexErrors, d.tokens.lexErrors === 1 ? "lexical error" : "lexical errors", d.tokens.lexErrors ? "bad" : "")}
      <span class="${eq.identical ? "" : "bad"}">${eq.identical ? "NFA, DFA and minimized DFA gave identical tokens" : "NFA, DFA and minimized DFA gave different tokens"}</span>
    </div>
    <div class="row" style="margin-bottom:10px">
      <input class="inp" id="tokSearch" placeholder="Filter tokens…" value="${esc(tokFilter)}" style="flex:1;min-width:140px;max-width:260px" aria-label="Filter tokens">
      <div class="chips" role="group" aria-label="Filter by token type">${Object.entries(counts).sort((a, b) => b[1] - a[1]).map(([k, n]) => `<button class="chip ${tokRule === k ? "sel" : ""}" data-rule="${esc(k)}" aria-pressed="${tokRule === k}">${esc(k)} <span class="n">${n}</span></button>`).join("")}</div>
    </div>
    <table class="grid"><thead><tr><th>#</th><th>Line:Col</th><th>Token</th><th>Lexeme</th></tr></thead><tbody>
      ${list.slice(0, tokLimit).map((t, i) => `<tr data-line="${t[2]}" data-col="${t[3]}"><td class="muted">${i + 1}</td><td class="muted">${t[2]}:${t[3]}</td><td><span class="tk" style="color:${TOK_COLOR[t[0]] || "var(--fg)"}">${esc(t[0])}</span></td><td class="lexeme">${esc(t[4].length > 80 ? t[4].slice(0, 77) + "…" : t[4]).replace(/\n/g, "↵")}</td></tr>`).join("")}
    </tbody></table>
    ${list.length > tokLimit ? `<div class="row" style="justify-content:center;margin-top:10px"><button class="btn" id="tokMore">Show ${Math.min(2000, list.length - tokLimit)} more (${fmt(list.length - tokLimit)} hidden)</button></div>` : ""}
    ${d.tokens.shown < d.tokens.count ? `<p class="note">${fmt(d.tokens.count - d.tokens.shown)} more tokens were scanned but not sent to the page.</p>` : ""}
    <details class="box" style="margin-top:18px"><summary>Scan time by automaton${speedup ? `<span class="sub">the DFA scanned ${speedup}× faster than simulating the NFA</span>` : ""}</summary><div class="inner">
      <div class="bars">
        ${[["Thompson NFA", eq.nfa.ms, ""], ["DFA", eq.dfa.ms, ""], ["Minimized DFA", eq.min.ms, "hi"]].map(([n, t, c]) =>
          `<div class="bar"><span>${n}</span><span class="track"><span class="fill ${c}" style="width:${Math.max(1.5, t / maxMs * 100)}%"></span></span><span style="text-align:right">${ms(t)}</span></div>`).join("")}
      </div>
      <p class="note">Same input, average of repeated runs. A DFA does one table lookup per character instead of tracking a set of NFA states.</p>
    </div></details>`;
  $("#tokSearch").oninput = e => { tokFilter = e.target.value.toLowerCase(); const pos = e.target.selectionStart; renderTokens(); const i = $("#tokSearch"); i.focus(); i.setSelectionRange(pos, pos); };
  $$("#v-tokens .chip").forEach(c => c.onclick = () => { tokRule = tokRule === c.dataset.rule ? "" : c.dataset.rule; renderTokens(); });
  $$("#v-tokens tbody tr").forEach(r => r.onclick = () => gotoLine(+r.dataset.line, +r.dataset.col));
  if ($("#tokMore")) $("#tokMore").onclick = () => { tokLimit += 2000; renderTokens(); };
}

/* ---- graph engine: layered layout + zoom/pan ---- */
function layered(nodes, edges, start, opts = {}) {
  // BFS levels from start; unreachable nodes go after
  const adj = new Map(nodes.map(n => [n.id, []]));
  for (const e of edges) adj.get(e.from)?.push(e.to);
  const level = new Map([[start, 0]]);
  const q = [start];
  while (q.length) { const u = q.shift(); for (const v of adj.get(u) || []) if (!level.has(v)) { level.set(v, level.get(u) + 1); q.push(v); } }
  let maxL = 0; for (const v of level.values()) maxL = Math.max(maxL, v);
  for (const n of nodes) if (!level.has(n.id)) level.set(n.id, ++maxL);
  const cols = new Map();
  for (const n of nodes) { const l = level.get(n.id); if (!cols.has(l)) cols.set(l, []); cols.get(l).push(n); }
  // order inside a column by the average position of parents (barycenter, one sweep)
  const pos = new Map();
  const dx = opts.dx || 96, dy = opts.dy || 64;
  [...cols.keys()].sort((a, b) => a - b).forEach(l => {
    const col = cols.get(l);
    col.forEach(n => { const ps = edges.filter(e => e.to === n.id && pos.has(e.from)).map(e => pos.get(e.from).y); n._b = ps.length ? ps.reduce((a, b) => a + b, 0) / ps.length : 0; });
    col.sort((a, b) => a._b - b._b);
    col.forEach((n, i) => pos.set(n.id, { x: l * dx + 40, y: (i - (col.length - 1) / 2) * dy }));
  });
  return pos;
}
function graphView(container, build) {
  // build(svgGroup) draws; returns {w,h,minY}
  container.innerHTML = `<div class="graph-tools"><button class="btn" data-z="full" title="Full screen (Esc to exit)">Full screen</button><button class="btn" data-z="in" title="Zoom in" aria-label="Zoom in">+</button><button class="btn" data-z="out" title="Zoom out" aria-label="Zoom out">−</button><button class="btn" data-z="fit" title="Fit to screen">Fit</button></div>
    <svg xmlns="http://www.w3.org/2000/svg" role="img"><defs><marker id="arr" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0 0L10 5L0 10z" fill="currentColor"/></marker></defs><g class="vp"></g></svg><div class="graph-legend"></div>`;
  const svg = container.querySelector("svg"), g = svg.querySelector(".vp");
  const box = build(g, container.querySelector(".graph-legend"));
  let t = { x: 0, y: 0, k: 1 };
  const apply = () => g.setAttribute("transform", `translate(${t.x},${t.y}) scale(${t.k})`);
  const fit = () => {
    const r = svg.getBoundingClientRect();
    const k = Math.min(1.6, Math.max(0.08, Math.min((r.width - 40) / Math.max(box.w, 1), (r.height - 40) / Math.max(box.h, 1))));
    t = { k, x: (r.width - box.w * k) / 2 - box.minX * k, y: (r.height - box.h * k) / 2 - box.minY * k };
    apply();
  };
  const zoom = (f, cx, cy) => { const r = svg.getBoundingClientRect(); cx ??= r.width / 2; cy ??= r.height / 2; const k = Math.min(4, Math.max(0.05, t.k * f)); t.x = cx - (cx - t.x) * k / t.k; t.y = cy - (cy - t.y) * k / t.k; t.k = k; apply(); };
  svg.addEventListener("wheel", e => { e.preventDefault(); const r = svg.getBoundingClientRect(); zoom(e.deltaY < 0 ? 1.12 : 1 / 1.12, e.clientX - r.left, e.clientY - r.top); }, { passive: false });
  let drag = null;
  svg.addEventListener("pointerdown", e => { if (e.target.closest(".node,.ast-node")) return; drag = { x: e.clientX - t.x, y: e.clientY - t.y }; svg.classList.add("drag"); svg.setPointerCapture(e.pointerId); });
  svg.addEventListener("pointermove", e => { if (!drag) return; t.x = e.clientX - drag.x; t.y = e.clientY - drag.y; apply(); });
  svg.addEventListener("pointerup", () => { drag = null; svg.classList.remove("drag"); });
  const setFull = on => {
    container.classList.toggle("full", on);
    document.body.classList.toggle("has-full", on);
    const b = container.querySelector('[data-z="full"]');
    b.textContent = on ? "Exit full screen" : "Full screen";
    requestAnimationFrame(fit);
  };
  container.querySelector(".graph-tools").onclick = e => {
    const z = e.target.closest("[data-z]")?.dataset.z;
    if (z === "in") zoom(1.25); if (z === "out") zoom(0.8); if (z === "fit") fit();
    if (z === "full") setFull(!container.classList.contains("full"));
  };
  if (container._esc) document.removeEventListener("keydown", container._esc);
  container._esc = e => { if (e.key === "Escape" && container.classList.contains("full")) setFull(false); };
  document.addEventListener("keydown", container._esc);
  if (container.classList.contains("full")) container.querySelector('[data-z="full"]').textContent = "Exit full screen";
  requestAnimationFrame(fit);
  return { fit, svg };
}
const SVGNS = "http://www.w3.org/2000/svg";
function el(tag, attrs, parent, text) { const e = document.createElementNS(SVGNS, tag); for (const k in attrs) e.setAttribute(k, attrs[k]); if (text != null) e.textContent = text; parent?.appendChild(e); return e; }
function edgePath(a, b, bend) {
  if (a === b) return { d: `M${a.x - 8} ${a.y - 16} C ${a.x - 30} ${a.y - 62}, ${a.x + 30} ${a.y - 62}, ${a.x + 8} ${a.y - 16}`, lx: a.x, ly: a.y - 52 };
  const dx = b.x - a.x, dy = b.y - a.y, len = Math.hypot(dx, dy) || 1;
  const r = 17, sx = a.x + dx / len * r, sy = a.y + dy / len * r, ex = b.x - dx / len * (r + 3), ey = b.y - dy / len * (r + 3);
  const back = b.x <= a.x;
  const off = back ? Math.max(40, Math.abs(dx) * 0.35) : bend;
  const mx = (sx + ex) / 2 - dy / len * off, my = (sy + ey) / 2 + dx / len * off + (back ? 30 : 0);
  return { d: `M${sx} ${sy} Q ${mx} ${my} ${ex} ${ey}`, lx: (sx + 2 * mx + ex) / 4, ly: (sy + 2 * my + ey) / 4 - 4 };
}

/* ---- automata ---- */
let autoGraph = null;
function currentAutomaton() {
  const d = state.data, r = d?.rules?.rules?.[state.auto.rule];
  if (!r) return null;
  const mode = state.auto.mode;
  if (mode === "nfa") {
    const nodes = r.nfa.states.map(s => ({ id: s.id, kind: s.type, accept: s.type === "match", label: String(s.id) }));
    const edges = [];
    for (const s of r.nfa.states) {
      if (s.type === "char" || s.type === "class") edges.push({ from: s.id, to: s.out, label: s.label, eps: false });
      if (s.type === "split") { if (s.out >= 0) edges.push({ from: s.id, to: s.out, label: "ε", eps: true }); if (s.out1 >= 0) edges.push({ from: s.id, to: s.out1, label: "ε", eps: true }); }
    }
    return { nodes, edges: edges.filter(e => e.to >= 0), start: r.nfa.start, rule: r };
  }
  const a = mode === "dfa" ? r.dfa : r.min;
  if (!a) return null;
  return { nodes: a.states.map(s => ({ id: s.id, accept: !!s.accept, label: String(s.id), members: s.members })), edges: a.edges.map(e => ({ ...e })), start: a.start, rule: r, total: a.total };
}
function renderAutomata() {
  const v = $("#v-automata"), d = state.data;
  if (!d?.rules) { v.innerHTML = empty("No automata yet", "Compile a program to build the NFA, DFA and minimized DFA from the lexical rules."); return; }
  delete v.dataset.stale;
  const rules = d.rules.rules, c = d.rules.combined || {};
  if (state.auto.rule >= rules.length) state.auto.rule = 0;
  const r = rules[state.auto.rule];
  const nfaN = r.nfa.states.length, dfaN = r.dfa?.total ?? 0, minN = r.min?.total ?? 0;
  const red = dfaN ? Math.round((1 - minN / dfaN) * 100) : 0;
  v.innerHTML = head("Finite automata", "automata", `<div class="seg" role="group" aria-label="Automaton">${["nfa", "dfa", "min"].map(m => `<button data-mode="${m}" class="${state.auto.mode === m ? "sel" : ""}" aria-pressed="${state.auto.mode === m}">${{ nfa: "NFA", dfa: "DFA", min: "Minimized DFA" }[m]}</button>`).join("")}</div>`) + `
    <div class="facts">
      ${fact(fmt(c.nfaStates), "NFA states (all rules)")}${fact(fmt(c.dfaStates), "DFA states")}
      ${fact(fmt(c.minStates), `minimized (${c.dfaStates ? Math.round((1 - c.minStates / c.dfaStates) * 100) : 0}% fewer)`)}
      ${fact(fmt(c.minTransitions), "transitions")}${fact(fmt(c.symbols), "input symbols")}${fact(ms((c.buildMs?.dfa || 0) + (c.buildMs?.min || 0)), "to build")}
    </div>
    <div class="chips" style="margin-bottom:14px" role="group" aria-label="Token rule">${rules.map((x, i) => `<button class="chip ${i === state.auto.rule ? "sel" : ""} ${x.neverWins ? "off never" : ""}" data-rule="${i}" aria-pressed="${i === state.auto.rule}" title="${esc(x.pattern)}${x.neverWins ? "\n(never wins: another rule always matches first)" : ""}">${esc(x.name)}</button>`).join("")}</div>
    <div class="rule-info">
      <div class="row"><b class="mono">${esc(r.name)}</b><span class="muted">priority ${r.priority}${r.skip ? " · skipped" : ""}</span><span class="spacer"></span>
        <span class="mono muted" style="font-size:12px">NFA ${nfaN} → DFA ${dfaN} → min ${minN}${dfaN ? ` (${red}% fewer)` : ""}</span></div>
      <div class="pattern">${esc(r.pattern.length > 300 ? r.pattern.slice(0, 300) + "…" : r.pattern)}</div>
      <div class="postfix">postfix: ${esc(r.postfix.map(p => p.v).join(" ").slice(0, 300))}</div>
    </div>
    <div class="graph-wrap" id="autoGraph"></div>
    <div class="row" style="margin-top:10px"><button class="btn" id="simToggle" aria-expanded="${!!state.sim}" aria-controls="simBar">Simulate input</button><span class="hint-line">Click a state to inspect it. Scroll to zoom, drag to pan.</span></div>
    <div class="sim ${state.sim ? "open" : ""}" id="simBar">
      <input class="inp mono" id="simInput" value="${esc(state.sim?.input ?? sampleFor(r))}" style="width:180px" aria-label="Input string">
      <button class="btn" id="simRun">Run</button>
      <button class="btn" id="simPrev" title="Previous step" aria-label="Previous step">◀</button><button class="btn" id="simNext" title="Next step" aria-label="Next step">▶</button><button class="btn" id="simPlay" title="Play all steps">Play</button>
      <div class="tape" id="simTape"></div><span class="verdict-line" id="simVerdict"></span>
    </div>
    <div class="inspector" id="inspector" style="display:none"></div>`;
  $$("#v-automata [data-mode]").forEach(b => b.onclick = () => { state.auto.mode = b.dataset.mode; state.sim = null; renderAutomata(); });
  $$("#v-automata .chip[data-rule]").forEach(b => b.onclick = () => { state.auto.rule = +b.dataset.rule; state.sim = null; renderAutomata(); });
  const A = currentAutomaton();
  if (!A) { $("#autoGraph").innerHTML = empty("This automaton is too large to draw"); return; }
  const tooBig = A.nodes.length > 450;
  autoGraph = graphView($("#autoGraph"), (g, legend) => {
    const nodes = tooBig ? A.nodes.slice(0, 450) : A.nodes;
    const ids = new Set(nodes.map(n => n.id));
    const edges = A.edges.filter(e => ids.has(e.from) && ids.has(e.to));
    const pos = layered(nodes, edges, A.start, { dx: state.auto.mode === "nfa" ? 78 : 110, dy: 60 });
    const eg = el("g", {}, g), ng = el("g", {}, g);
    const pair = new Map();
    for (const e of edges) {
      const key = e.from + "-" + e.to, rev = e.to + "-" + e.from;
      const bend = pair.has(rev) ? 18 : 0; pair.set(key, 1);
      const p = edgePath(pos.get(e.from), pos.get(e.to), bend);
      const gg = el("g", { class: "edge" + (e.eps ? " eps" : ""), "data-from": e.from, "data-to": e.to }, eg);
      el("path", { d: p.d, "marker-end": "url(#arr)", style: "color:var(--faint)" }, gg);
      el("text", { x: p.lx, y: p.ly }, gg, e.label.length > 14 ? e.label.slice(0, 13) + "…" : e.label);
    }
    // start arrow
    const s0 = pos.get(A.start);
    if (s0) el("path", { d: `M${s0.x - 52} ${s0.y} L${s0.x - 20} ${s0.y}`, "marker-end": "url(#arr)", style: "stroke:var(--accent);stroke-width:2;color:var(--accent)" }, eg);
    for (const n of nodes) {
      const p = pos.get(n.id);
      const gg = el("g", { class: `node${n.accept ? " accept" : ""}${n.id === A.start ? " start" : ""}${n.kind === "split" ? " split" : ""}`, "data-id": n.id, transform: `translate(${p.x},${p.y})`, tabindex: 0 }, ng);
      el("circle", { r: 16 }, gg);
      if (n.accept) el("circle", { r: 12, class: "outer", style: "fill:none" }, gg);
      el("text", {}, gg, n.label);
      el("title", {}, gg, `state ${n.id}${n.accept ? " (accepting)" : ""}`);
    }
    let minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
    for (const p of pos.values()) { minX = Math.min(minX, p.x); maxX = Math.max(maxX, p.x); minY = Math.min(minY, p.y); maxY = Math.max(maxY, p.y); }
    legend.innerHTML = `<span>→ start: ${A.start}</span><span>◎ accepting</span>${state.auto.mode === "nfa" ? "<span>- - ε move</span>" : ""}${tooBig ? `<span class="warn">showing 450 of ${A.nodes.length} states</span>` : ""}${A.total > A.nodes.length ? `<span class="warn">exported ${A.nodes.length} of ${A.total} states</span>` : ""}`;
    return { minX: minX - 70, minY: minY - 70, w: maxX - minX + 140, h: maxY - minY + 140 };
  });
  $$("#autoGraph .node").forEach(n => n.onclick = () => inspectState(A, +n.dataset.id));
  $("#simToggle").onclick = () => { const open = $("#simBar").classList.toggle("open"); $("#simToggle").setAttribute("aria-expanded", open); if (open) $("#simInput").focus(); };
  $("#simRun").onclick = runSim;
  $("#simInput").onkeydown = e => { if (e.key === "Enter") runSim(); };
  $("#simPrev").onclick = () => simStep(-1);
  $("#simNext").onclick = () => simStep(1);
  $("#simPlay").onclick = () => { clearInterval(state.simTimer); if (!state.sim) return; state.sim.i = 0; paintSim(); state.simTimer = setInterval(() => { if (!simStep(1)) clearInterval(state.simTimer); }, 650); };
  if (state.sim) paintSim();
}
function sampleFor(r) {
  const ex = (state.data?.tokens?.list || []).find(t => t[0] === r.name && t[4].length < 24 && !t[4].includes("\n"));
  return ex ? ex[4] : "";
}
function inspectState(A, id) {
  $$("#autoGraph .node").forEach(n => n.classList.toggle("picked", +n.dataset.id === id));
  const n = A.nodes.find(x => x.id === id), out = A.edges.filter(e => e.from === id);
  const box = $("#inspector");
  box.style.display = "block";
  let html = `<b>State ${id}</b>${id === A.start ? " · start" : ""}${n.accept ? ` · <span style="color:var(--ok)">accepts ${esc(A.rule.name)}</span>` : ""}<br>`;
  if (state.auto.mode === "nfa") {
    const s = A.rule.nfa.states[id];
    html += s.type === "split" ? "ε-state: moves without reading input (to " + [s.out, s.out1].filter(x => x >= 0).join(" and ") + ")" : s.type === "match" ? "match state: the input read so far is a token" : `reads ${esc(s.label)} then goes to ${s.out}`;
  } else {
    html += `${out.length} outgoing transition${out.length === 1 ? "" : "s"}: ${out.map(e => `${esc(e.label)}→${e.to}`).join(", ") || "none (dead end)"}<br>`;
    if (n.members) html += `${state.auto.mode === "dfa" ? "NFA states in this set (its ε-closure)" : "DFA states merged into this one"}: {${n.members.filter(x => x >= 0).join(", ")}${n.members.includes(-1) ? ", …" : ""}}`;
  }
  box.innerHTML = html;
}
async function runSim() {
  const r = state.data.rules.rules[state.auto.rule];
  const input = $("#simInput").value;
  try {
    const res = await api("/api/simulate", { rule: r.name, input, automaton: state.auto.mode, language: state.data.source.language, rules: state.data.source.customRules ? $("#rulesText").value : undefined });
    state.sim = { ...res, i: 0, input };
    paintSim();
  } catch (e) { toast(e.message, "error"); }
}
function simStep(dir) {
  if (!state.sim) return false;
  const n = state.sim.steps.length;
  const ni = Math.min(n - 1, Math.max(0, state.sim.i + dir));
  if (ni === state.sim.i) return false;
  state.sim.i = ni;
  paintSim();
  return true;
}
function paintSim() {
  const S = state.sim, step = S.steps[S.i];
  $("#simBar").classList.add("open");
  $("#simTape").innerHTML = [...S.input].map((ch, i) => `<span class="${i < step.pos ? "done" : ""} ${i === step.pos - 1 ? "now" : ""}">${esc(ch === " " ? "␣" : ch)}</span>`).join("") || `<span>ε</span>`;
  const active = new Set(S.automaton === "nfa" ? step.active : step.state >= 0 ? [step.state] : []);
  $$("#autoGraph .node").forEach(n => n.classList.toggle("active", active.has(+n.dataset.id)));
  const prev = S.i > 0 ? S.steps[S.i - 1] : null;
  $$("#autoGraph .edge").forEach(e => {
    const f = +e.dataset.from, t = +e.dataset.to;
    let hot = false;
    if (prev && S.automaton !== "nfa") hot = f === prev.state && t === step.state;
    if (prev && S.automaton === "nfa") hot = prev.active.includes(f) && step.moved.includes(t);
    e.classList.toggle("hot", hot);
  });
  const last = S.i === S.steps.length - 1;
  const dead = S.automaton === "nfa" ? !step.active.length : step.state < 0;
  $("#simVerdict").innerHTML = `step ${S.i}/${S.steps.length - 1}${S.automaton === "nfa" ? ` · ${step.active.length} active state${step.active.length === 1 ? "" : "s"}` : ""} · ` +
    (dead ? `<span style="color:var(--err)">no move: stuck</span>` : step.accepting ? `<span style="color:var(--ok)">accepting</span>` : "not accepting") +
    (last || dead ? ` · longest match ${S.longestMatch < 0 ? "none" : S.longestMatch + " char" + (S.longestMatch === 1 ? "" : "s")} · whole input ${S.accepted ? "<b style='color:var(--ok)'>accepted</b>" : "<b style='color:var(--err)'>rejected</b>"}` : "");
}

/* ---- syntax ---- */
const KIND_CLASS = k => /Function|Class|Param|VarDecl|DeclGroup|StructDef|EnumDef|Enumerator|Import|Directive|Global|Program/.test(k) ? "k-decl"
  : /If|While|For|DoWhile|ForEach|Return|Block|Switch|Case|Break|Continue|Try|Throw|Assert|Pass|ExprStmt/.test(k) ? "k-stmt"
  : /Lit$/.test(k) ? "k-lit" : /Unsupported|Truncated/.test(k) ? "k-bad" : "k-expr";
let astCollapsed = new WeakSet(), astTreeRoot = null;
function renderSyntax() {
  const v = $("#v-syntax"), d = state.data;
  if (!d) { v.innerHTML = empty("No syntax tree yet", "Compile to parse the program."); return; }
  delete v.dataset.stale;
  if (!d.ast) { v.innerHTML = head("Syntax & semantics", "syntax") + lexOnlyNote(d); return; }
  if (astTreeRoot !== d.ast) {
    astTreeRoot = d.ast; astCollapsed = new WeakSet();
    const walk = (n, depth) => { if (n.kids) { if (depth >= 3) astCollapsed.add(n); n.kids.forEach(k => walk(k, depth + 1)); } };
    walk(d.ast, 0);
  }
  const syms = d.symbols || [];
  const perr = d.diagnostics.filter(x => x.stage === "parser" && x.severity === "error").length;
  const serr = d.diagnostics.filter(x => x.stage === "semantic" && x.severity === "error").length;
  v.innerHTML = head("Syntax & semantics", "syntax", `<button class="btn" id="astExpand">Expand all</button><button class="btn" id="astCollapse">Collapse</button>`) + `
    <div class="facts">
      ${fact(fmt(d.astNodes), "AST nodes")}${fact(perr, perr === 1 ? "syntax error" : "syntax errors", perr ? "bad" : "")}
      ${fact(serr, serr === 1 ? "semantic error" : "semantic errors", serr ? "bad" : "")}${fact(fmt(syms.length), "symbols")}
    </div>
    <div class="graph-wrap" id="astGraph" style="height:520px"></div>
    <p class="hint-line" style="margin:8px 0 0">Click a node to expand or collapse it and highlight its source line. Outline colours: purple declarations · blue statements · grey-blue expressions · green literals · red dashed not translated.</p>
    <details class="box" style="margin-top:16px"><summary>Symbol table <span class="sub">${plural(syms.length, "entry", "entries")}</span></summary><div class="inner">
      <table class="grid"><thead><tr><th>Name</th><th>Kind</th><th>Type</th><th>Scope</th><th>Line</th><th>Uses</th></tr></thead><tbody>
      ${syms.slice(0, 800).map(s => `<tr data-line="${s.line}"><td>${esc(s.name)}</td><td class="muted">${esc(s.kind)}</td><td>${esc(s.type)}</td><td class="muted">${esc(s.scope)}</td><td class="muted">${s.line || ""}</td><td class="muted">${s.uses}</td></tr>`).join("")}
      </tbody></table></div></details>
    <details class="box"><summary>Parser trace <span class="sub">decisions recorded by the parser</span></summary><div class="inner"><pre class="out">${esc((d.trace.parser || []).join("\n") || "(none)")}</pre></div></details>`;
  drawAst();
  $("#astExpand").onclick = () => { astCollapsed = new WeakSet(); drawAst(); };
  $("#astCollapse").onclick = () => { const w = (n, dp) => { if (n.kids) { if (dp >= 1) astCollapsed.add(n); n.kids.forEach(k => w(k, dp + 1)); } }; astCollapsed = new WeakSet(); w(d.ast, 0); drawAst(); };
  $$("#v-syntax tbody tr").forEach(r => r.onclick = () => gotoLine(+r.dataset.line));
}
function drawAst() {
  const d = state.data;
  graphView($("#astGraph"), (g, legend) => {
    // tidy-ish tree: leaves get consecutive x, parents centered over children
    let nextX = 0, count = 0;
    const W = 150, H = 66;
    const layout = (n, depth) => {
      count++;
      const kids = n.kids && !astCollapsed.has(n) && count < 2500 ? n.kids : [];
      const placed = kids.map(k => layout(k, depth + 1));
      const x = placed.length ? (placed[0].x + placed[placed.length - 1].x) / 2 : nextX++ * W;
      return { n, x, y: depth * H, kids: placed };
    };
    const tree = layout(d.ast, 0);
    let minX = 1e9, maxX = -1e9, maxY = 0;
    const eg = el("g", {}, g), ng = el("g", {}, g);
    const draw = t => {
      minX = Math.min(minX, t.x); maxX = Math.max(maxX, t.x); maxY = Math.max(maxY, t.y);
      for (const k of t.kids) { el("path", { d: `M${t.x} ${t.y + 15} C ${t.x} ${t.y + 40}, ${k.x} ${k.y - 30}, ${k.x} ${k.y - 15}`, style: "fill:none;stroke:var(--line-strong);stroke-width:1.2" }, eg); draw(k); }
      const n = t.n, hidden = n.kids && astCollapsed.has(n);
      const gg = el("g", { class: "node ast-node " + KIND_CLASS(n.kind), transform: `translate(${t.x},${t.y})`, tabindex: 0 }, ng);
      const label = n.label ? (n.label.length > 16 ? n.label.slice(0, 15) + "…" : n.label) : "";
      el("rect", { x: -64, y: -15, width: 128, height: 30, rx: 6 }, gg);
      el("text", { y: label ? -5 : 0 }, gg, n.kind);
      if (label) el("text", { y: 8, class: "sub" }, gg, label + (n.type ? " : " + n.type : ""));
      if (hidden) el("text", { x: 54, y: -10, class: "more" }, gg, "+" + n.kids.length);
      el("title", {}, gg, `${n.kind}${n.label ? " " + n.label : ""}${n.type ? " : " + n.type : ""} (line ${n.line})`);
      gg.addEventListener("click", () => {
        if (n.kids) { if (astCollapsed.has(n)) astCollapsed.delete(n); else astCollapsed.add(n); drawAst(); }
        if (n.line) gotoLine(n.line, n.col);
      });
    };
    draw(tree);
    legend.textContent = `${count} node${count === 1 ? "" : "s"} shown of ${d.astNodes}`;
    return { minX: minX - 80, minY: -40, w: maxX - minX + 160, h: maxY + 80 };
  });
}

/* ---- intermediate code ---- */
function diffClasses(orig, opt, records) {
  const optIds = new Map();
  for (const f of opt) for (const i of f.code) optIds.set(i.id, i.t);
  const moved = new Set(records.filter(r => r.pass === "Loop-Invariant Code Motion").map(r => r.id));
  return id => !optIds.has(id) ? "removed" : moved.has(id) ? "moved" : null;
}
/* "field access .x is not translated to three-address code; kept as ..." -> "Field access .x" */
const constructName = m => { const s = m.replace(/ is not translated to three-address code.*$/s, ""); return s.charAt(0).toUpperCase() + s.slice(1); };
function partialNotice(d) {
  const t = d.tac, n = t.opaque;
  const items = d.diagnostics.filter(x => x.stage === "tac" && x.category === "Unsupported Construct");
  const runBlocked = d.run?.original?.status === "unsupported";
  return callout("warn", "!", "Partial translation",
    `${plural(n, "construct")} ${n === 1 ? "was" : "were"} not translated to three-address code. ${n === 1 ? "It is" : "They are"} preserved as opaque operations, so nothing is dropped and the optimizer works around ${n === 1 ? "it" : "them"}.${runBlocked ? " The program cannot be run because execution reaches one of them." : ""}` +
    (items.length ? `<details><summary>Show ${items.length === 1 ? "the construct" : `the ${items.length} constructs`}</summary><ul class="construct-list">${items.map(x =>
      `<li><button data-line="${x.line}" data-col="${x.col}"><span class="ln">line ${x.line}</span><span>${esc(constructName(x.message))}</span></button></li>`).join("")}</ul></details>` : ""));
}
function renderCode() {
  const v = $("#v-code"), d = state.data;
  if (!d) { v.innerHTML = empty("No intermediate code yet", "Compile to generate three-address code."); return; }
  if (!d.tac) { v.innerHTML = head("Intermediate code & optimization", "code") + lexOnlyNote(d); return; }
  const t = d.tac, o = d.opt;
  const fnames = ["*", ...t.original.map(f => f.func)];
  if (!fnames.includes(state.fn)) state.fn = "*";
  const pick = fs => state.fn === "*" ? fs : fs.filter(f => f.func === state.fn);
  const cls = diffClasses(t.original, t.optimized, o.records);
  const lines = (fs, side) => {
    let n = 0;
    return fs.map(f => `<div class="fn"><span class="ln"></span>function ${esc(f.func)}(${f.params.map(esc).join(", ")}):</div>` + f.code.map(i => {
      n++;
      let c = i.k === "label" ? "label" : i.k === "opaque" ? "opaque" : "";
      if (side === "o") { const k = cls(i.id); if (k) c += " " + k; else if (o.records.some(r => r.id === i.id && r.after)) c += " changed"; }
      else if (o.records.some(r => r.id === i.id && r.pass === "Loop-Invariant Code Motion")) c += " moved";
      else if (o.records.some(r => r.id === i.id && r.after)) c += " changed";
      return `<div class="${c}" data-id="${i.id}" data-line="${i.line}" title="source line ${i.line}"><span class="ln">${n}</span>${i.k === "label" ? "" : "  "}${esc(i.t)}</div>`;
    }).join("")).join("");
  };
  const cut = t.originalCount > 4000 && state.fn === "*";
  const showO = cut ? pick(t.original).slice(0, 25) : pick(t.original), showP = cut ? pick(t.optimized).slice(0, 25) : pick(t.optimized);
  const recs = o.records.filter(r => state.fn === "*" || r.func === state.fn);
  const byPass = {};
  for (const r of recs) (byPass[r.pass] ||= []).push(r);
  const cntO = pick(t.original).reduce((a, f) => a + f.code.filter(i => i.k !== "label").length, 0);
  const cntP = pick(t.optimized).reduce((a, f) => a + f.code.filter(i => i.k !== "label").length, 0);
  const passes = state.passes || o.passes.filter(p => p.enabled).map(p => p.key);
  v.innerHTML = head("Intermediate code & optimization", "code", `<select class="sel" id="fnSel" aria-label="Function">${fnames.map(f => `<option value="${esc(f)}" ${f === state.fn ? "selected" : ""}>${f === "*" ? "All functions" : esc(f)}</option>`).join("")}</select>`) +
    (t.complete && !t.opaque ? "" : partialNotice(d)) + `
    <div class="facts">
      ${fact(fmt(cntO), "original instructions")}${fact(fmt(cntP), `optimized (${cntO ? ((cntO - cntP) / cntO * 100).toFixed(1) : 0}% fewer)`)}${fact(fmt(recs.length), recs.length === 1 ? "transformation" : "transformations")}
    </div>
    <details class="box" style="margin:0 0 6px"><summary>Optimization passes <span class="sub">${passes.length} of ${o.passes.length} on · ${plural(o.rounds, "round")} · ${ms(o.ms)}</span></summary><div class="inner">
      <div class="chips" role="group" aria-label="Optimization passes">${o.passes.map(p => `<button class="chip ${passes.includes(p.key) ? "sel" : "off"}" data-pass="${p.key}" aria-pressed="${passes.includes(p.key)}" title="${plural(p.count, "change")}">${esc(p.name)} <span class="n">${p.count}</span></button>`).join("")}</div>
      <p class="note">By default every pass runs, and each one only applies changes that are safe. Switching a pass recompiles the program. <button class="btn sm" id="passAll">All</button> <button class="btn sm" id="passNone">None</button></p>
    </div></details>
    <div class="legend"><span><i class="removed"></i>removed</span><span><i class="changed"></i>rewritten</span><span><i class="moved"></i>moved out of a loop</span>${t.opaque ? `<span><i class="opaque"></i>opaque (not translated)</span>` : ""}</div>
    <div class="compare">
      <div class="tacbox"><h4><span>Original TAC</span><span class="mono">${fmt(cntO)}</span></h4><div class="tac" id="tacO">${lines(showO, "o0")}</div></div>
      <div class="tacbox"><h4><span>Optimized TAC</span><span class="mono">${fmt(cntP)}</span></h4><div class="tac" id="tacP">${lines(showP, "o")}</div></div>
    </div>
    ${cut ? `<p class="note">Large program: showing the first 25 functions. Choose a function above to see it fully.</p>` : ""}
    <h3>Applied optimizations</h3>
    <p class="note" style="margin:0 0 8px">From the optimizer's own records. Click one to find it in the code.</p>
    ${Object.keys(byPass).length ? Object.entries(byPass).map(([p, rs]) => `<details class="box" ${Object.keys(byPass).length <= 3 ? "open" : ""}><summary>${esc(p)} <span class="sub">${rs.length}</span></summary><div class="inner records">
      ${rs.slice(0, 200).map(r => `<div class="rec" data-id="${r.id}" data-line="${r.line}" tabindex="0" role="button"><span class="tag">B${r.block}</span><span class="chg"><del>${esc(r.before || "")}</del>${r.after != null && r.after !== r.before ? ` → <ins>${esc(r.after)}</ins>` : r.after == null ? ` → <ins>(removed)</ins>` : ` → <ins>moved before the loop</ins>`}</span><span class="why">${esc(r.why)}</span><span class="where">${esc(r.func)} · line ${r.line}</span></div>`).join("")}
      ${rs.length > 200 ? `<p class="note">${rs.length - 200} more not shown.</p>` : ""}</div></details>`).join("") : `<p class="muted" style="font-size:13px">No transformation was safe to apply${passes.length ? "" : " (all passes are switched off)"}.</p>`}
    <details class="box" style="margin-top:14px"><summary>Control-flow graph</summary><div class="inner">
      <div class="row" style="margin-bottom:8px"><select class="sel" id="cfgFn" aria-label="Function for the control-flow graph">${t.optimized.map(f => `<option ${f.func === (state.fn === "*" ? (t.optimized.find(x => x.func === "main" || x.func.endsWith(".main") || x.func === "<module>")?.func) : state.fn) ? "selected" : ""}>${esc(f.func)}</option>`).join("")}</select>
      <div class="seg"><button data-cfg="opt" class="${state.cfgSide !== "orig" ? "sel" : ""}">Optimized</button><button data-cfg="orig" class="${state.cfgSide === "orig" ? "sel" : ""}">Original</button></div><span class="hint-line">Blue edges are loop back edges.</span></div>
      <div class="graph-wrap" id="cfgGraph" style="height:480px"></div></div></details>
    <details class="box"><summary>TAC generation trace</summary><div class="inner"><pre class="out">${esc((d.trace.tac || []).join("\n") || "(none)")}</pre></div></details>`;
  $("#fnSel").onchange = e => { state.fn = e.target.value; renderCode(); };
  $$("#v-code [data-pass]").forEach(b => b.onclick = () => { const s = new Set(passes); s.has(b.dataset.pass) ? s.delete(b.dataset.pass) : s.add(b.dataset.pass); state.passes = [...s]; compile(); });
  $("#passAll").onclick = () => { state.passes = o.passes.map(p => p.key); compile(); };
  $("#passNone").onclick = () => { state.passes = []; compile(); };
  $$("#v-code .construct-list button").forEach(b => b.onclick = () => gotoLine(+b.dataset.line, +b.dataset.col));
  $$("#v-code .rec").forEach(r => { r.onclick = () => flashTac(+r.dataset.id, +r.dataset.line); r.onkeydown = e => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); r.click(); } }; });
  $$("#v-code .tac div[data-id]").forEach(r => r.onclick = () => { flashTac(+r.dataset.id, +r.dataset.line); });
  $("#cfgFn").onchange = drawCfg;
  $$("#v-code [data-cfg]").forEach(b => b.onclick = () => { state.cfgSide = b.dataset.cfg; $$("#v-code [data-cfg]").forEach(x => x.classList.toggle("sel", x === b)); drawCfg(); });
  $("#v-code details:has(#cfgGraph)")?.addEventListener("toggle", e => { if (e.target.open) drawCfg(); });
}
function flashTac(id, line) {
  $$("#v-code .tac div.flash").forEach(x => x.classList.remove("flash"));
  $$(`#v-code .tac div[data-id="${id}"]`).forEach(x => { x.classList.add("flash"); x.scrollIntoView({ block: "nearest", behavior: "smooth" }); });
  if (line) gotoLine(line);
}
function drawCfg() {
  const d = state.data, name = $("#cfgFn").value;
  const f = (state.cfgSide === "orig" ? d.cfgOriginal : d.cfg).find(x => x.func === name);
  if (!f) return;
  const back = new Set(f.backEdges.map(([u, h]) => u + "-" + h));
  graphView($("#cfgGraph"), (g, legend) => {
    const nodes = f.blocks.map(b => ({ id: b.id })), edges = [];
    for (const b of f.blocks) for (const s of b.succ) edges.push({ from: b.id, to: s });
    // top-to-bottom: use the layered layout with axes swapped
    const pos0 = layered(nodes, edges.filter(e => !back.has(e.from + "-" + e.to)), 0, { dx: 1, dy: 1 });
    const BW = 230, LH = 15;
    const heights = Object.fromEntries(f.blocks.map(b => [b.id, Math.min(b.code.length, 14) * LH + 26]));
    const rows = {};
    for (const [id, p] of pos0) (rows[p.x] ||= []).push([id, p.y]);
    const pos = new Map();
    let y = 0;
    for (const r of Object.keys(rows).map(Number).sort((a, b) => a - b)) {
      const items = rows[r].sort((a, b) => a[1] - b[1]);
      const h = Math.max(...items.map(([id]) => heights[id]));
      items.forEach(([id], i) => pos.set(id, { x: (i - (items.length - 1) / 2) * (BW + 40), y }));
      y += h + 46;
    }
    const eg = el("g", {}, g), ng = el("g", {}, g);
    let minX = 1e9, maxX = -1e9;
    for (const e of edges) {
      const a = pos.get(e.from), b = pos.get(e.to);
      if (!a || !b) continue;
      const isBack = back.has(e.from + "-" + e.to);
      const ax = a.x, ay = a.y + heights[e.from], bx = b.x, by = b.y;
      const d2 = isBack ? `M${ax + BW / 2} ${ay - 10} C ${ax + BW / 2 + 70} ${ay}, ${bx + BW / 2 + 70} ${by}, ${bx + BW / 2} ${by + 12}` : `M${ax} ${ay} C ${ax} ${ay + 25}, ${bx} ${by - 25}, ${bx} ${by - 2}`;
      el("path", { d: d2, "marker-end": "url(#arr)", style: `fill:none;stroke:${isBack ? "var(--accent)" : "var(--faint)"};stroke-width:1.4;color:${isBack ? "var(--accent)" : "var(--faint)"}` }, eg);
    }
    for (const b of f.blocks) {
      const p = pos.get(b.id);
      if (!p) continue;
      minX = Math.min(minX, p.x); maxX = Math.max(maxX, p.x);
      const gg = el("g", { class: "node", transform: `translate(${p.x - BW / 2},${p.y})` }, ng);
      el("rect", { width: BW, height: heights[b.id], rx: 7 }, gg);
      el("text", { x: 10, y: 12, style: "text-anchor:start;fill:var(--accent)" }, gg, "B" + b.id);
      b.code.slice(0, 14).forEach((line, i) => el("text", { x: 10, y: 28 + i * LH, style: "text-anchor:start;font-weight:400;font-size:10.5px" }, gg, (line.length > 32 ? line.slice(0, 31) + "…" : line) + (i === 13 && b.code.length > 14 ? ` (+${b.code.length - 14})` : "")));
    }
    legend.textContent = `${f.blocks.length} basic blocks · ${edges.length} edges · ${f.backEdges.length} loop back edge${f.backEdges.length === 1 ? "" : "s"}`;
    return { minX: minX - BW / 2 - 90, minY: -30, w: maxX - minX + BW + 180, h: y + 30 };
  });
}

/* ---- run & compare ---- */
function runVerdict(d) {
  const r = d.run;
  if (d.source.customRules || !d.tac) return lexOnlyNote(d);
  if (!r) return callout("neutral", "–", "Not executed", `<span class="muted">${esc(d.stages.find(s => s.id === "run")?.summary || "")}</span>`);
  if (r.equivalent === null) return callout("neutral", "?", "Not executable", `<span class="muted">${esc(r.original.message)}</span>`);
  if (r.equivalent) return callout("ok", "✓", "Original and optimized TAC matched for this execution.",
    `<span class="muted">Same output and the same ${r.original.status === "ok" ? "normal termination" : "exception"}; ${fmt(r.original.steps)} → ${fmt(r.optimized.steps)} executed instructions (${r.original.steps ? Math.round((1 - r.optimized.steps / r.original.steps) * 100) : 0}% fewer). One matching run is evidence for this input, not a proof for every input.</span>`);
  return callout("error", "✕", "Original and optimized TAC behaved differently in this execution.",
    `<span class="muted">Original: ${esc(r.original.status)} · optimized: ${esc(r.optimized.status)}. ${esc(r.optimized.message)}</span>`);
}
function renderRun() {
  const v = $("#v-run"), d = state.data;
  if (!d) { v.innerHTML = empty("Nothing has run yet", "Compile a complete program to execute its TAC."); return; }
  const r = d.run, m = d.metrics;
  const stageMs = [["Lexing", m.ms.lex], ["Parsing", m.ms.parse], ["TAC + semantics", m.ms.tac], ["Optimization", m.ms.opt], ["Run (orig + opt)", (r?.original.ms || 0) + (r?.optimized.ms || 0)]];
  const maxS = Math.max(...stageMs.map(x => x[1])) || 1;
  v.innerHTML = head("Run & compare", "run") + runVerdict(d) + `
    ${r && r.original.status !== "unsupported" ? `<h3 style="margin-top:4px">Program output <span class="muted" style="font-weight:400;font-size:12px">from executing the original TAC${r.original.status === "exception" ? " · ended with an exception" : ""}</span></h3>
      <pre class="out">${esc(r.original.stdout || "(no output)")}${r.original.status !== "ok" ? `\n<span style="color:var(--err)">${esc(r.original.message)}</span>` : ""}</pre>
      ${r.original.undefinedBehaviour ? `<p class="note" style="color:var(--warn)">C undefined behaviour was detected while running (${esc(r.original.ubFirst)}). Real compilers may produce different results here.</p>` : ""}` : ""}
    <div class="facts" style="margin-top:16px">
      ${fact(fmt(m.lines), "source lines")}${fact(fmt(m.tokens), "tokens")}${fact(fmt(m.astNodes), "AST nodes")}
      ${fact(`${fmt(m.tacOriginal)}→${fmt(m.tacOptimized)}`, "TAC instructions")}${fact((m.peakMemoryKB / 1024).toFixed(1) + " MB", "peak memory")}
    </div>
    <details class="box"><summary>Time per stage <span class="sub">measured in the C compiler</span></summary><div class="inner"><div class="bars">
      ${stageMs.map(([n, t]) => `<div class="bar"><span>${n}</span><span class="track"><span class="fill hi" style="width:${Math.max(1, t / maxS * 100)}%"></span></span><span style="text-align:right">${ms(t)}</span></div>`).join("")}
    </div></div></details>
    ${r ? `<details class="box"><summary>Execution details</summary><div class="inner mono" style="font-size:12px;line-height:1.7">
      original: ${esc(r.original.status)}, ${fmt(r.original.steps)} steps, ${ms(r.original.ms)}${r.original.message ? " — " + esc(r.original.message) : ""}<br>
      optimized: ${esc(r.optimized.status)}, ${fmt(r.optimized.steps)} steps, ${ms(r.optimized.ms)}${r.optimized.message ? " — " + esc(r.optimized.message) : ""}<br>
      entry points: ${(d.tac?.entry || []).map(esc).join(" → ")}</div></details>` : ""}
    <details class="box"><summary>Lexer trace <span class="sub">path through the minimized DFA, first tokens</span></summary><div class="inner"><pre class="out">${esc((d.lexTrace || []).slice(0, 120).map(t => `${String(t.line).padStart(4)}:${String(t.col).padEnd(4)} ${t.rule.padEnd(10)} ${JSON.stringify(t.text).slice(0, 30).padEnd(30)} ${t.path.join("→")}${t.skip ? "  (skipped)" : ""}`).join("\n"))}</pre></div></details>`;
}

/* ---- diagnostics ---- */
function renderDiagnostics() {
  const ds = state.data?.diagnostics || [];
  const n = s => ds.filter(d => d.severity === s).length;
  $("#c-err").textContent = n("error"); $("#c-warn").textContent = n("warning"); $("#c-note").textContent = n("note");
  $("#consoleMsg").textContent = state.data ? (ds.length ? "Select a message to jump to its line" : "No problems found") : "Compile to see errors and warnings";
  const order = { error: 0, warning: 1, note: 2 };
  $("#consoleBody").innerHTML = ds.slice().sort((a, b) => order[a.severity] - order[b.severity] || a.line - b.line).map(d =>
    `<button class="diag" data-line="${d.line}" data-col="${d.col}"><span class="sev ${d.severity}"></span><span class="loc">${d.line ? `${d.line}:${d.col}` : "—"} · ${esc(d.stage)}</span><span><span class="cat ${d.severity}">${esc(d.category)}</span>${esc(d.message)}${d.hint ? `<span class="hint">${esc(d.hint)}</span>` : ""}</span></button>`).join("");
  $$("#consoleBody .diag").forEach(x => x.onclick = () => gotoLine(+x.dataset.line, +x.dataset.col));
  if (n("error")) toggleConsole(true);
}

/* new source loaded: old results no longer describe it -- wait for Compile */
function clearResults() {
  state.data = null; state.sim = null; state.focusLine = 0; state.passes = null; state.compiled = null; state.failed = null;
  renderAll();
  renderEditor();
  code.focus();
}

/* ------------------------------------------------------------------ compile */
/* With the local server the API is HTTP; without it (GitHub Pages) the same calls run in the browser. */
async function api(path, body) {
  path = path.replace(/^\//, "");
  if (state.inBrowser) return BrowserCompiler.call(path, body);
  const res = await fetch(path, { method: body ? "POST" : "GET", headers: body ? { "Content-Type": "application/json" } : {}, body: body ? JSON.stringify(body) : undefined });
  const data = await res.json().catch(() => ({ error: "bad response from the server" }));
  if (!res.ok) throw new Error(data.error || res.statusText);
  return data;
}
function setCompiling(on) {
  state.compiling = on;
  $("#compileBtn").disabled = on;
  $("#compileBtn").classList.toggle("busy", on);
  $("#compileLabel").textContent = on ? "Compiling…" : "Compile";
  renderStatus();
}
async function compile() {
  if (state.compiling) return;
  if (!code.value.trim()) { toast("Type, paste or open some code first", "warn"); code.focus(); return; }
  const inputs = currentInputs();
  setCompiling(true);
  renderPipeline(true);
  $("#pipeline").classList.remove("stale");
  const t0 = performance.now();
  try {
    const custom = $("#customRules").checked;
    const body = { code: code.value, language: $("#lang").value, filename: state.filename, passes: state.passes || undefined, rules: custom ? $("#rulesText").value : undefined };
    const d = await api("/api/compile", body);
    state.data = d;
    state.compiled = inputs;
    state.failed = null;
    state.focusLine = 0;
    state.sim = null;
    astTreeRoot = null;
    if ($("#lang").value === "auto") syncRulesText();
    setCompiling(false);
    renderAll();
    renderEditor();
    const errs = d.diagnosticCounts.errors;
    const secs = ((performance.now() - t0) / 1000).toFixed(2);
    toast(`${d.source.customRules ? "Custom rules" : d.source.language}: ${fmt(d.metrics.lines)} lines compiled in ${secs}s`, errs ? "error" : "ok");
  } catch (e) {
    state.failed = e.message.includes("Failed to fetch") ? "the server is not running" : e.message;
    setCompiling(false);
    renderPipeline(false);
    refreshStale();
    toast(e.message.includes("Failed to fetch") ? "The server is not running: start it with ./run.sh" : e.message, "error");
  }
}

/* ------------------------------------------------------------------ export */
/* Exports describe the last compile, so they are offered only while it matches the editor. */
function updateExportMenu() {
  const none = !state.data, stale = !!staleReason();
  const note = $("#exportNote");
  note.hidden = !(none || stale);
  note.className = "menu-note" + (stale ? " stale" : "");
  note.textContent = none ? "Compile first to export results." : stale ? "Results are out of date. Compile again to export." : "";
  $$("#exportList [data-export]").forEach(b => b.disabled = none || stale || (b.dataset.export === "tac" && !state.data?.tac) || (b.dataset.export === "ast" && !state.data?.ast));
}
function download(name, text, type = "text/plain") {
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([text], { type }));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}
$$("#exportList [data-export]").forEach(b => b.onclick = () => {
  const d = state.data, k = b.dataset.export;
  closeMenus(true);
  if (!d || staleReason()) { toast("Compile again first", "warn"); return; }
  const base = (d.source.name || "program").split("/").pop().replace(/\.[^.]+$/, "");
  const tacText = side => d.tac[side].map(f => `function ${f.func}(${f.params.join(", ")}):\n` + f.code.map(i => (i.k === "label" ? "" : "    ") + i.t).join("\n")).join("\n\n");
  if (k === "json") download(base + ".workbench.json", JSON.stringify(d, null, 1), "application/json");
  if (k === "tokens") download(base + ".tokens.csv", "line,col,token,lexeme\n" + d.tokens.list.map(t => `${t[2]},${t[3]},${t[0]},"${t[4].replace(/"/g, '""')}"`).join("\n"), "text/csv");
  if (k === "automata") download(base + ".automata.json", JSON.stringify({ combined: d.rules?.combined, rules: d.rules?.rules.map(r => ({ name: r.name, pattern: r.pattern, nfa: r.nfa, dfa: r.dfa, minimized: r.min })) }, null, 1), "application/json");
  if (k === "ast") download(base + ".ast.json", JSON.stringify(d.ast, null, 1), "application/json");
  if (k === "tac") { if (!d.tac) return toast("No TAC for this run", "warn"); download(base + ".tac.txt", "== original ==\n" + tacText("original") + "\n\n== optimized ==\n" + tacText("optimized")); }
  if (k === "diags") download(base + ".diagnostics.txt", d.diagnostics.map(x => `${d.source.name}:${x.line}:${x.col}: ${x.severity} [${x.category}] ${x.message}${x.hint ? "\n    hint: " + x.hint : ""}`).join("\n") || "no diagnostics");
  if (k === "svg") {
    const svg = $(".view.sel .graph-wrap svg");
    if (!svg) return toast("Open the Automata, Syntax or control-flow graph view first", "warn");
    const clone = svg.cloneNode(true);
    const cs = getComputedStyle(root);
    clone.setAttribute("style", `background:${cs.getPropertyValue("--sunken")}`);
    let s = new XMLSerializer().serializeToString(clone);
    s = s.replace(/var\(--([\w-]+)\)/g, (_, v) => cs.getPropertyValue("--" + v).trim());
    let rules = [];
    try { rules = [...[...document.styleSheets].find(x => x.href?.endsWith("workbench.css"))?.cssRules || []]; } catch {}
    const css = rules.map(r => r.cssText).filter(t => /\.node|\.edge|\.ast-node/.test(t)).join("\n").replace(/var\(--([\w-]+)\)/g, (_, v) => cs.getPropertyValue("--" + v).trim());
    s = s.replace("<defs>", `<defs><style>${css}</style>`);
    download(base + ".diagram.svg", s, "image/svg+xml");
  }
  if (k === "report") {
    const sum = compileSummary();
    const st = d.stages.map((s, i) => `| ${STAGES[i]?.[1] || s.name} | ${s.status} | ${s.summary || ""} |`).join("\n");
    const recs = d.opt ? d.opt.passes.filter(p => p.count).map(p => `- **${p.name}**: ${p.count}`).join("\n") : "";
    const runLine = !d.run ? "Not executed." : d.run.equivalent === true ? "Original and optimized TAC matched for this execution."
      : d.run.equivalent === false ? "Original and optimized TAC behaved differently in this execution." : `Not executable: ${d.run.original.message}`;
    download(base + ".report.md", `# Compilation report: ${d.source.name}\n\nRegLex Compiler Workbench, an educational compiler for supported subsets of C, Java, and Python.\n\n` +
      `Result: **${sum.label}**${sum.detail ? ` (${sum.detail})` : ""} · ${d.source.customRules ? "custom lexical rules" : d.source.language} · ${d.metrics.lines} lines · ${d.tokens.count} tokens\n\n| Stage | Status | Summary |\n|---|---|---|\n${st}\n\n` +
      `## Automata\nNFA ${d.rules?.combined?.nfaStates} states → DFA ${d.rules?.combined?.dfaStates} → minimized ${d.rules?.combined?.minStates}. NFA, DFA and minimized DFA gave identical tokens for this input: ${d.lexEquivalence.identical ? "yes" : "no"}.\n\n` +
      (d.tac ? `## Optimization\nTAC ${d.tac.originalCount} → ${d.tac.optimizedCount} instructions.${d.tac.opaque ? ` ${d.tac.opaque} construct(s) were not translated and are preserved as opaque operations.` : ""}\n\n${recs}\n\n` : "") +
      (d.tac ? `## Run comparison\n${runLine}\n\n` + (d.run && d.run.original.status !== "unsupported" ? `\`\`\`\n${d.run.original.stdout}\n\`\`\`\n\n` : "") : "") +
      `## Diagnostics\n${d.diagnostics.map(x => `- ${x.line}:${x.col} ${x.severity} [${x.category}] ${x.message}`).join("\n") || "none"}\n`, "text/markdown");
  }
});

/* ------------------------------------------------------------------ boot */
async function boot() {
  renderPipeline(false);
  renderAll();
  $("#lang").value = store.get("lang", "auto");
  try {
    const health = await fetch("api/health").then(r => r.ok ? r.json() : null).catch(() => null);
    if (!health?.languages && typeof BrowserCompiler === "object" && await BrowserCompiler.available().catch(() => false)) state.inBrowser = true;
    state.examples = await api("/api/examples");
    if (state.examples.tests) {
      const t = state.examples.tests, note = $("#tests");
      note.hidden = false;
      note.classList.toggle("bad", t.passed !== t.total);
      note.textContent = `${t.passed}/${t.total} tests passed`;
      note.title = `Last test run ${t.date}: ` + Object.entries(t.sections).map(([k, [a, b]]) => `${k} ${a}/${b}`).join(", ");
    }
    if (state.inBrowser && BrowserCompiler.mode === "examples") {
      const note = $("#tests");
      note.hidden = false;
      note.textContent = "Online demo · examples only";
      note.title = "Pick a program from Open → Load example (or the language box) and press Compile. To compile your own code, run the workbench on your computer with ./run.sh.";
    }
  } catch (e) { toast("Start the server (./run.sh) to compile", "warn"); }
  const saved = store.get("code", "");
  const savedRules = store.get("rules", "");
  if (savedRules) $("#rulesText").value = savedRules;
  if (saved) code.value = saved;
  else if (state.examples) {   // first visit only: start with a sample instead of an empty editor
    const l = $("#lang").value === "auto" ? "C" : $("#lang").value;
    const ex = state.examples.examples.find(x => x.file === SAMPLE[l]);
    if (ex) loadExample(ex);
  }
  renderEditor();
  syncRulesText();
  showView(state.view);
}
boot();
