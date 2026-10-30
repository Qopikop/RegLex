/* Runs build/rcc compiled to WebAssembly (web/wasm/, made by web/build_wasm.sh).
   Each request gets a fresh compiler instance: {id, args, files} -> {id, out, err, code} or {id, error}. */
"use strict";
importScripts("wasm/rcc.js");

const binary = fetch("wasm/rcc.wasm").then(r => {
  if (!r.ok) throw new Error("the browser compiler (wasm/rcc.wasm) is missing");
  return r.arrayBuffer();
});

onmessage = async e => {
  const { id, args, files } = e.data;
  let out = "", err = "";
  try {
    const M = await createRcc({ wasmBinary: await binary, noInitialRun: true,
                                print: s => { out += s + "\n"; }, printErr: s => { err += s + "\n"; } });
    for (const [path, text] of Object.entries(files || {})) {
      M.FS.mkdirTree(path.slice(0, path.lastIndexOf("/")));
      M.FS.writeFile(path, text);
    }
    let code = 0;
    try { code = M.callMain(args); }
    catch (x) { if (x && x.name === "ExitStatus") code = x.status; else throw x; }
    postMessage({ id, out, err, code });
  } catch (x) {
    postMessage({ id, out, err, error: String(x && x.message || x) });
  }
};
