#!/usr/bin/env bash
# Builds the files that let the workbench run with no server (GitHub Pages):
#   web/wasm/rcc.js + rcc.wasm   the compiler (src/) compiled to WebAssembly, rules/ embedded
#   web/examples.json            what GET /api/examples returns (examples, default rules, test summary)
# Needs Emscripten (brew install emscripten). Run from anywhere: web/build_wasm.sh
set -euo pipefail
cd "$(dirname "$0")/.."

SRC="src/util.c src/regex.c src/automata.c src/lexer.c src/ast.c src/parse.c src/parse_c.c src/parse_py.c src/ir.c src/value.c src/lower.c src/opt.c src/interp.c src/rcc.c"
mkdir -p web/wasm
# The interpreter allows 20,000 nested calls, so the stack is large; the heap grows as needed.
emcc -O2 -std=gnu11 -Wno-unused-parameter -Wno-sign-compare $SRC -lm -o web/wasm/rcc.js \
    -sMODULARIZE=1 -sEXPORT_NAME=createRcc -sENVIRONMENT=worker -sINVOKE_RUN=0 -sEXIT_RUNTIME=1 \
    -sEXPORTED_RUNTIME_METHODS=callMain,FS -sFORCE_FILESYSTEM=1 \
    -sSTACK_SIZE=256MB -sINITIAL_MEMORY=320MB -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB \
    --embed-file rules@/rules

python3 -c 'import json, sys; sys.path.insert(0, "visualize"); import serve; json.dump(serve.examples(), open("web/examples.json", "w"), ensure_ascii=False)'
ls -lh web/wasm web/examples.json
