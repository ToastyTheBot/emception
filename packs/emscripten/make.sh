#!/bin/bash
set -e

if [ -d emscripten ]; then
    # nothing to do here
    exit
fi

SRC=$(dirname $0)
SRC=$(realpath "$SRC")
# CWD is build/packs/emscripten (set by package.sh); build root is two levels up.
BUILD_ROOT=$(realpath ../..)

# emscripten 3.1.74 — matches the LLVM 20 llvm-box and the emsdk:3.1.74 tools. Its
# compiler is src/compiler.mjs (ESM, top-level await, import.meta); it is bundled
# below and run by quicknode's ESM support (node:fs/path/url/vm/assert provided by
# quicknode). See docs/emception-upgrade-spike.md.
VER=3.1.74
curl --silent --output emscripten.zip --location https://github.com/emscripten-core/emscripten/archive/refs/tags/$VER.zip
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

# Avoid invalidating the pre-built cache we ship: disable Cache.erase() in check_sanity
# (the shipped emsdk cache records a different LLVM path than emception's /usr/bin, so
# sanity always thinks "config changed").
sed -i -E 's/^(\s*)[Cc]ache\.erase\(\)/\1pass # cache.erase() disabled by emception/' tools/shared.py || true
if grep -q "cache.erase() disabled by emception" tools/shared.py; then
    echo "emception: patched shared.py check_sanity (disabled Cache.erase)"
else
    echo "emception: WARNING could not disable Cache.erase in shared.py"
fi

# emception's python.wasm ships a minimal stdlib WITHOUT urllib.request (it pulls in
# http/email/ssl, which aren't built). emscripten 3.1.74's tools/ports/__init__.py
# imports `urlopen` at module load for port downloads — which emception never does
# (fully offline, pre-built sysroot). Make the import optional so em++ can load.
perl -0pi -e 's/^from urllib\.request import urlopen$/try:\n    from urllib.request import urlopen\nexcept ModuleNotFoundError:\n    urlopen = None/m' tools/ports/__init__.py
if grep -q "urlopen = None" tools/ports/__init__.py; then
    echo "emception: patched ports/__init__.py (optional urllib.request import)"
else
    echo "emception: WARNING could not patch ports/__init__.py urllib import"
fi

# Install dependencies (but not development dependencies)
npm i --only=prod

# Bundle the ESM compiler entry (src/compiler.mjs) + its local .mjs imports
# (utility/modules/parseTools/jsifier) into one self-contained ES module, leaving the
# node: builtins external (quicknode provides them via its module loader). This keeps
# the ordered top-level `await import(...)` and import.meta semantics that emscripten
# relies on. Runtime-read data (settings.js, settings_internal.js, library*.js) is NOT
# bundled and stays in src/.
npx --yes esbuild@0.24.0 src/compiler.mjs --bundle --format=esm --platform=neutral '--external:node:*' --outfile=src/compiler.bundle.mjs
mv src/compiler.bundle.mjs src/compiler.mjs
echo "emception: bundled src/compiler.mjs (single ESM, node:* external)"

# quicknode's QuickJS (2021 Bellard) has NO top-level-await support. The bundle's only
# TLA is esbuild's synchronously-resolving `await Promise.resolve().then(() => (init_X(),
# X_exports))` wrappers for compiler.mjs's ordered dynamic imports. Strip the await (the
# thens resolve synchronously) so the file parses in QuickJS module mode — the node:
# imports still resolve via quicknode's module loader.
perl -0pi -e 's/await Promise\.resolve\(\)\.then\(\(\) => \((init_\w+\(\), \w+_exports)\)\)/($1)/g' src/compiler.mjs
if grep -q "await Promise.resolve().then" src/compiler.mjs; then
    echo "emception: WARNING residual top-level await in compiler.mjs"
else
    echo "emception: stripped top-level await from bundled compiler.mjs"
fi

# quicknode runs the compiler with a single shared global (no node:vm realm isolation).
# emscripten relies on the macro context seeing the settings as ARRAYS (macros do
# EXPORTED_RUNTIME_METHODS.filter(...).map(...)) while module functions see them as Sets
# (WASM_EXPORTS.has(...)). With one global the Set conversion wins and breaks the macros.
# Make the 7 converted settings array-like objects that ALSO have .has/.add/.delete/.size,
# satisfying both. (ArraySet returns a real Array, so .filter/.map/.reduce stay native.)
perl -0pi -e '
  my $k = "function ArraySet(iter){var arr=iter?Array.from(iter):[];Object.defineProperties(arr,{has:{value:function(v){return arr.includes(v);}},add:{value:function(v){if(!arr.includes(v))arr.push(v);return arr;}},delete:{value:function(v){var i=arr.indexOf(v);if(i>=0)arr.splice(i,1);return i>=0;}},size:{get:function(){return arr.length;}}});return arr;}\n";
  s/^(EXPORTED_FUNCTIONS = )new Set\(/$k$1new ArraySet(/m;
  s/^(WASM_EXPORTS|SIDE_MODULE_EXPORTS|INCOMING_MODULE_JS_API|ALL_INCOMING_MODULE_JS_API|EXPORTED_RUNTIME_METHODS|WEAK_IMPORTS)( = )new Set\(/$1$2new ArraySet(/mg;
' src/compiler.mjs
if grep -q "new ArraySet(EXPORTED_RUNTIME_METHODS)" src/compiler.mjs; then
    echo "emception: patched settings Set->ArraySet (array-like with .has)"
else
    echo "emception: WARNING ArraySet patch not applied"
fi

# tools/preprocessor.mjs (the HTML-shell step, run as a separate node tool) is also ESM
# with local imports (../src/utility.mjs, dynamic parseTools/modules), a bare `assert`
# import, and top-level await. Bundle + TLA-strip it the same way as compiler.mjs.
npx --yes esbuild@0.24.0 tools/preprocessor.mjs --bundle --format=esm --platform=neutral \
    '--external:node:*' --external:assert --external:fs --external:path --external:url --external:vm --external:os \
    --outfile=tools/preprocessor.bundle.mjs
mv tools/preprocessor.bundle.mjs tools/preprocessor.mjs
perl -0pi -e 's/await Promise\.resolve\(\)\.then\(\(\) => \((init_\w+\(\), \w+_exports)\)\)/($1)/g' tools/preprocessor.mjs
echo "emception: bundled + TLA-stripped tools/preprocessor.mjs"

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

CONTAINER_ID=$(docker create emscripten/emsdk:$VER)
docker cp $CONTAINER_ID:/emsdk/upstream/emscripten/cache ./cache
docker rm $CONTAINER_ID

# Pre-compile .py -> .pyc with the NATIVE CPython 3.12 (build/cpython-native, same
# commit as python.wasm so the magic matches). python.wasm traps (RuntimeError:
# unreachable) trying to *compile* the ~84KB emcc.py; shipping unchecked-hash .pyc
# makes it unmarshal bytecode instead of compiling source.
if [ -x "$BUILD_ROOT/cpython-native/python" ]; then
    "$BUILD_ROOT/cpython-native/python" -m compileall --invalidation-mode unchecked-hash -q . \
        || echo "emception: WARNING compileall reported errors (continuing)"
    echo "emception: pre-compiled .py -> .pyc with native cpython"
else
    echo "emception: WARNING $BUILD_ROOT/cpython-native/python not found; skipping .pyc precompile"
fi

popd

# Consolidate: the frontend worker loads a single /emscripten pack, so do NOT
# split the sysroot into ~300 separate packages.
# node "$SRC/split_packages.cjs" | bash
