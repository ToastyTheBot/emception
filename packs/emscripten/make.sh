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
