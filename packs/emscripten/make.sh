#!/bin/bash

if [ -d emscripten ]; then
    # nothing to do here
    exit
fi

SRC=$(dirname $0)
SRC=$(realpath "$SRC")

# 3.1.30 is the newest emscripten release that (a) still expects LLVM 16 — matching
# emception's llvm-box (EXPECTED_LLVM_VERSION 16.0; 3.1.31 bumps to 17) — and (b)
# still ships the CommonJS src/compiler.js (3.1.44+ switches to ESM compiler.mjs,
# which quicknode/QuickJS cannot load). Going newer requires rebuilding LLVM
# (17/18/20), an 8GB+ RAM link that OOMs the shared host. See docs/emception-upgrade-spike.md.
curl --silent --output emscripten.zip --location https://github.com/emscripten-core/emscripten/archive/refs/tags/3.1.30.zip
unzip -q emscripten.zip
rm emscripten.zip
mv emscripten-* emscripten

pushd emscripten/

cp $SRC/config ./.emscripten

# We won't support closure-compiler, remove it from the dependencies
cat package.json \
    | jq '. | del(.dependencies["google-closure-compiler"])' \
    | jq '. | del(.dependencies["html-minifier-terser"])' \
    > _package.json
mv _package.json package.json

# Patch emscripten to avoid invalidating the pre-built cache we ship: comment out the
# Cache.erase() call in check_sanity (the shipped emsdk cache records a different
# LLVM path than emception's /usr/bin, so sanity always thinks "config changed").
sed -i -E 's/^(\s*)[Cc]ache\.erase\(\)/\1pass # cache.erase() disabled by emception/' tools/shared.py || true
if grep -q "cache.erase() disabled by emception" tools/shared.py; then
    echo "emception: patched shared.py check_sanity (disabled Cache.erase)"
else
    echo "emception: WARNING could not disable Cache.erase in shared.py"
fi

# quicknode (QuickJS) has no Node 'vm' builtin. emscripten's compiler.js and
# preprocessor.js do `global.vm = require('vm')` and call vm.runInThisContext(code),
# which is equivalent to indirect eval (global scope, returns the completion value).
# Shim it so those tools run under quicknode.
for f in src/compiler.js tools/preprocessor.js; do
    perl -0pi -e "s/global\.vm = require\('vm'\);/try { global.vm = require('vm'); } catch (e) { const indirectEval = eval; global.vm = { runInThisContext: function (code, opts) { return indirectEval(code); } }; }/" "\$f"
done
if grep -q "indirectEval" src/compiler.js; then
    echo "emception: patched vm shim into compiler.js + preprocessor.js"
else
    echo "emception: WARNING vm shim not applied"
fi

# Install dependencies (but nor development dependencies)
npm i --only=prod

# Remove a bunch of things we won't use
rm -Rf \
    ./.circleci \
    ./.github \
    ./cmake \
    ./site \
    ./test \
    ./third_party/closure-compiler \
    ./third_party/jni \
    ./third_party/ply \
    ./third_party/websockify \
    ./tools/websocket_to_posix_proxy \
    ./*.bat

CONTAINER_ID=$(docker create emscripten/emsdk:3.1.30)
docker cp $CONTAINER_ID:/emsdk/upstream/emscripten/cache ./cache
docker rm $CONTAINER_ID

popd

# Consolidate: the frontend worker loads a single /emscripten pack, so do NOT
# split the sysroot into ~300 separate packages.
# node "$SRC/split_packages.cjs" | bash

# Pre-compile the emscripten .py to .pyc with the NATIVE CPython 3.12
# (build/cpython-native/python, same commit as python.wasm so the magic matches).
# python.wasm traps (RuntimeError: unreachable) trying to *compile* the ~84KB
# emcc.py; shipping unchecked-hash .pyc makes it unmarshal bytecode instead.
#   build/cpython-native/python -m compileall --invalidation-mode unchecked-hash -q emscripten/