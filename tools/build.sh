#!/usr/bin/env bash
# The normal Linux build. Makes sure tools/rexglue/linux-amd64 holds the ReXGlue runtime, GPU plugin and code generator
# built from this branch's patches, building them with tools/build-patched-rexruntime.sh when it does not (git and
# network access the first time), then configures and builds the matching preset into out/build/<preset>.
#
#   tools/build.sh                  linux-amd64-release
#   tools/build.sh relwithdebinfo   linux-amd64-relwithdebinfo
#
# Needs the extracted linux-amd64 SDK in tools/rexglue, generated/default and generated/tmnt_r (tools/codegen.ps1), and
# game/extracted/default.xex and TMNT_r.xex, which the build's codegen step reads. See docs/linux.md.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$ROOT/tools/rexglue/linux-amd64"
PATCHED="$ROOT/tools/build-patched-rexruntime.sh"

fail() { echo "ERROR: $*" >&2; exit 1; }

case "${1:-release}" in
  release) CONFIG=Release ;;
  relwithdebinfo) CONFIG=RelWithDebInfo ;;
  debug) fail "the stock Debug runtime has no patch 0082, so it cannot load the TMNT_r module; use relwithdebinfo" ;;
  *) fail "usage: $0 [release|relwithdebinfo]" ;;
esac
PRESET="linux-amd64-${1:-release}"
[ "$(uname -s)" = Linux ] || fail "builds on Linux only"
[ -d "$SDK/lib/cmake/rexglue" ] || fail "missing $SDK (extract rexglue-sdk-0.10.0-linux-amd64.zip into tools/rexglue)"
[ -f "$ROOT/generated/default/sources.cmake" ] || fail "generated/default is missing (run tools/codegen.ps1 first)"

if ! bash "$PATCHED" --check "$SDK" >/dev/null 2>&1; then
  echo "== Building the patched ReXGlue runtime and GPU plugin"
  bash "$PATCHED" "$SDK"
fi
bash "$PATCHED" --install-generator "$SDK"

cd "$ROOT"
cmake --preset "$PRESET"
cmake --build --preset "$PRESET"
echo "Built out/build/$PRESET/tmnt_turtles_in_time_reshelled"
