#!/usr/bin/env bash
# Static and negative checks of the Linux release: the pinned AppImage inputs, and on Linux a fixture AppDir staged and
# scanned by the real policy functions from stand-in binaries, then damaged in the ways the release must refuse. With
# TMNT_APPIMAGE_TOOLS naming a folder that holds appimagetool-*-x86_64.AppImage and a built runtime, a fixture AppImage is
# made too, and started through AppRun with and without FUSE. Needs no network and builds nothing of the project.
#
#   tools/test-appimage-release.sh [ROOT]
set -uo pipefail

ROOT="$(cd "${1:-$(dirname "$0")/..}" && pwd)"
PATCH_DIR="$ROOT/patches/appimage-runtime"

EXPECTED_RUNTIME_TAG=20251108
EXPECTED_RUNTIME_COMMIT=dd6cebedcbddde9c82f89b011e8e1d40b6e43868
EXPECTED_RUNTIME_SHA256=9e458da0e30e2dd06a206af0f345d98a872bf7c1ce69eba31d44e139488e4ab8
EXPECTED_ALPINE_SHA256=55ea3e5a7c2c35e6268c5dcbb8e45a9cd5b0e372e7b4e798499a526834f7ed90
EXPECTED_PATCH_SHA256=9abff6b867bb89a228df024035f28b229ca1149ea1cc172ebc10b2744b798a4e
EXPECTED_APPIMAGETOOL_VERSION=1.9.1
EXPECTED_APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0

failures=0
checks=0
ok() { checks=$((checks + 1)); echo "PASS: $1"; }
bad() { checks=$((checks + 1)); failures=$((failures + 1)); echo "FAIL: $1"; }
expect() {  # what condition-exit-status
  if [ "$2" = 0 ]; then ok "$1"; else bad "$1"; fi
}

fail() { echo "ERROR: $*" >&2; exit 1; }
# shellcheck source=../patches/appimage-runtime/baseline.env
. "$PATCH_DIR/baseline.env"
# shellcheck source=linux-policy.sh
. "$ROOT/tools/linux-policy.sh"

[ "$RUNTIME_TAG" = "$EXPECTED_RUNTIME_TAG" ] && [ "$RUNTIME_COMMIT" = "$EXPECTED_RUNTIME_COMMIT" ]
expect "the runtime source is pinned to type2-runtime $EXPECTED_RUNTIME_TAG (${EXPECTED_RUNTIME_COMMIT:0:7})" $?

[ "$RUNTIME_SHA256" = "$EXPECTED_RUNTIME_SHA256" ] && [ "$ALPINE_SHA256" = "$EXPECTED_ALPINE_SHA256" ]
expect "the reviewed runtime and Alpine root file system SHA-256 are pinned" $?

[ "$(tr -d '\r' < "$PATCH_DIR/0001-extract-and-run-when-fuse-fails.patch" | sha256sum | cut -d' ' -f1)" = "$EXPECTED_PATCH_SHA256" ] &&
  [ "$(find "$PATCH_DIR" -name '*.patch' | wc -l)" = 1 ]
expect "the runtime patch is the reviewed extract-and-run patch and the only one" $?

[ "$APPIMAGETOOL_VERSION" = "$EXPECTED_APPIMAGETOOL_VERSION" ] && [ "$APPIMAGETOOL_SHA256" = "$EXPECTED_APPIMAGETOOL_SHA256" ]
expect "appimagetool $EXPECTED_APPIMAGETOOL_VERSION is pinned by SHA-256" $?

grep -q 'releases/download/\$APPIMAGETOOL_VERSION/' "$ROOT/tools/package-linux.sh"
expect "appimagetool is downloaded from its versioned release" $?

if grep -n -i -E 'releases/download/(continuous|latest)|/latest/download' "$ROOT"/tools/*.sh | grep -v 'test-appimage-release.sh' | grep -q .; then
  bad "no script downloads a continuous or latest AppImage runtime"
else
  ok "no script downloads a continuous or latest AppImage runtime"
fi

calls="$(grep -h -E 'ARCH=x86_64 "\$(TOOL|tool)"' "$ROOT/tools/package-linux.sh" "$ROOT/tools/test-appimage-release.sh")"
[ "$(wc -l <<<"$calls")" = 2 ] && [ "$(grep -c -- '--runtime-file' <<<"$calls")" = 2 ]
expect "the release build and the fixture AppImage are the only appimagetool calls and both pass --runtime-file" $?

grep -q -- '--check-image' "$ROOT/tools/package-linux.sh"
expect "the release build checks the runtime at the front of the finished AppImage" $?

grep -q 'APPIMAGE_EXTRACT_AND_RUN\|extract-and-run\|extract_and_run' "$PATCH_DIR/0001-extract-and-run-when-fuse-fails.patch"
expect "the runtime patch keeps the extract-and-run fallback" $?

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo content > "$work/tool"
good="$(sha256sum "$work/tool" | cut -d' ' -f1)"
( download "file://$work/missing" "$work/tool" "$good" ) >/dev/null 2>&1
expect "a download that already matches its pinned SHA-256 is accepted" $?

( download "file://$work/missing" "$work/tool" "$(printf 'x%.0s' {1..64})" ) >/dev/null 2>&1
[ $? -ne 0 ]
expect "appimagetool with the wrong SHA-256 is refused" $?

( policy_self_test ) >/dev/null 2>&1
expect "the release policy self-test passes" $?

fixture_needs() {
  [ "$(uname -s)" = Linux ] || return 1
  grep -q GLIBCXX_3.4.32 /usr/lib/x86_64-linux-gnu/libstdc++.so.6 2>/dev/null || return 1
  [ -f /usr/share/doc/libstdc++6/copyright ] || return 1
  local tool
  for tool in clang-20 readelf objcopy strip ldd; do command -v "$tool" >/dev/null || return 1; done
}

if ! fixture_needs; then
  echo "NOT RUN: the fixture AppDir needs Linux with clang-20, binutils and the GCC 13 runtime (tools/setup-linux-build-env.sh)"
  echo "$checks checks, $failures failures"
  [ "$failures" = 0 ]
  exit
fi

bash "$ROOT/tools/build-appimage-runtime.sh" --check /bin/true >/dev/null 2>&1
[ $? -ne 0 ]
expect "a program that is not the patched runtime is refused" $?

bash "$ROOT/tools/build-appimage-runtime.sh" --check-image /bin/true >/dev/null 2>&1
[ $? -ne 0 ]
expect "an AppImage that does not start with the reviewed runtime is refused" $?

policy_init "$ROOT"
bin="$work/bin"
mkdir -p "$bin/assets/launcher"
printf 'stub\n' > "$bin/$BIN_NAME.toml"
printf 'png\n' | tee "$bin/assets/launcher/header-art.png" > "$bin/assets/launcher/sponsor-heart.png"
echo 'int fixture_runtime(void) { return 0; }' > "$work/runtime.c"
clang-20 -g -shared -fPIC -Wl,-soname,librexruntime.so -o "$bin/librexruntime.so" "$work/runtime.c"
clang-20 -g -shared -fPIC -Wl,-soname,librexgpu-xenos.so "-Wl,-rpath,\$ORIGIN:\$ORIGIN/../lib" -o "$bin/librexgpu-xenos.so" "$work/runtime.c"
clang-20 -g -shared -fPIC "-Wl,-rpath,\$ORIGIN" -o "$bin/$MODULE_NAME" "$work/runtime.c"
cat > "$work/program.c" <<EOF
#include <stdio.h>
#include <stdlib.h>
int fixture_runtime(void);
const char* application_id = "$APP_ID";
int main(int argc, char** argv) {
  const char* appimage = getenv("APPIMAGE");
  printf("program %d %s %s\n", argc, argc > 1 ? argv[1] : "-", appimage ? appimage : "-");
  return argc > 2 ? 7 : fixture_runtime();
}
EOF
clang-20 -g "-Wl,-rpath,\$ORIGIN" -L"$bin" -o "$bin/$BIN_NAME" "$work/program.c" -lrexruntime

appdir="$work/AppDir"
symbols="$work/symbols"
( stage_appdir "$appdir" "$bin" "$symbols" "$ROOT" ) > "$work/stage.log" 2>&1
expect "stage_appdir stages the fixture" $?
[ -f "$symbols/$BIN_NAME.debug" ] && [ -f "$symbols/$MODULE_NAME.debug" ]
expect "the program's and the module's debug information stays in the symbols folder" $?
if readelf -S -W "$appdir/usr/bin/$BIN_NAME" "$appdir/usr/bin/$MODULE_NAME" | grep -qF ' .debug_'; then false; else true; fi
expect "the staged program and module hold no debug sections" $?
holds_built_libraries "$bin" "$appdir/usr/bin"
expect "the staged runtime and GPU plugin are the built ones, stripped" $?
( scan_appdir "$appdir" fixture "$work/nowhere" ) > "$work/scan.log" 2>&1
expect "scan_appdir accepts the fixture" $?
bash "$ROOT/tools/check-glibc-baseline.sh" --max "$MAX_GLIBC" "$appdir" > "$work/glibc.log" 2>&1
expect "the fixture AppDir meets the glibc $MAX_GLIBC baseline" $?

refused() {  # what mutation-command...
  local what="$1" copy="$work/mutated"
  shift
  rm -rf "$copy"
  cp -a "$appdir" "$copy"
  ( cd "$copy" && "$@" ) >/dev/null 2>&1
  ( scan_appdir "$copy" fixture "$work/nowhere" ) >/dev/null 2>&1
  [ $? -ne 0 ]
  expect "scan_appdir refuses $what" $?
}
refused "a stray default.xex" touch usr/bin/default.xex
refused "TMNT_r.xex" touch usr/bin/TMNT_r.xex
refused "a user_data folder" mkdir usr/bin/user_data
refused "settings.toml" touch usr/bin/settings.toml
refused "a log file" touch usr/bin/run.log
refused "a debug file" touch usr/bin/program.debug
refused "an unexpected library" cp usr/bin/librexruntime.so usr/bin/libextra.so
refused "a missing guest module" rm "usr/bin/$MODULE_NAME"
refused "a RenderDoc library" touch usr/bin/librenderdoc.so
refused "a bundled Vulkan loader" touch usr/lib/libvulkan.so.1
refused "a Vulkan layer manifest" touch usr/bin/VkLayer_khronos_validation.json
refused "a Windows binary" sh -c 'printf MZ > usr/bin/assets/launcher/header-art.png'
refused "a DEBUG_SAULO marker" sh -c 'printf DEBUG_SAULO >> usr/bin/assets/launcher/header-art.png'
refused "an Xbox package" sh -c 'printf LIVE > usr/bin/assets/launcher/header-art.png'
refused "an Xbox executable" sh -c 'printf XEX2 > usr/bin/assets/launcher/header-art.png'
refused "a local path" sh -c "printf %s '$work/nowhere' >> usr/bin/$BIN_NAME.toml"
refused "a UTF-16 local path" sh -c "printf %s '$work/nowhere' | sed 's/./&\\x00/g' >> usr/bin/$BIN_NAME.toml"
refused "a program without the application id" sh -c "sed -i 's/io.github.saulob/io.github.nobody/' usr/bin/$BIN_NAME"
refused "a desktop file with another Exec" sed -i 's/^Exec=.*/Exec=\/home\/dev\/program/' "$APP_ID.desktop"
refused "a program with debug sections" cp "$bin/$BIN_NAME" "usr/bin/$BIN_NAME"

tools="${TMNT_APPIMAGE_TOOLS:-}"
tool="$(ls "$tools"/appimagetool-*-x86_64.AppImage 2>/dev/null | head -1)"
runtime="$(ls "$tools"/runtime-*-x86_64 2>/dev/null | grep -v '\.' | head -1)"
if [ -z "$tool" ] || [ -z "$runtime" ]; then
  echo "NOT RUN: the fixture AppImage (set TMNT_APPIMAGE_TOOLS to a folder with appimagetool and a built runtime)"
else
  chmod +x "$tool"
  image="$work/Fixture-x86_64.AppImage"
  ARCH=x86_64 "$tool" --appimage-extract-and-run --no-appstream --runtime-file "$runtime" "$appdir" "$image" > "$work/tool.log" 2>&1
  expect "appimagetool makes the fixture AppImage with the pinned runtime" $?
  chmod 755 "$image"
  bash "$ROOT/tools/build-appimage-runtime.sh" --check "$image" >/dev/null 2>&1
  expect "the fixture AppImage carries the patched runtime" $?
  bash "$ROOT/tools/build-appimage-runtime.sh" --check-image "$image" >/dev/null 2>&1
  expect "the fixture AppImage starts with the reviewed runtime" $?
  ( cd "$work" && "$image" --appimage-extract >/dev/null 2>&1 )
  ( scan_appdir "$work/squashfs-root" "extracted fixture" "$work/nowhere" ) >/dev/null 2>&1
  expect "the extracted fixture AppImage passes the same scan" $?
  output="$(APPIMAGE_EXTRACT_AND_RUN=1 "$image" one 2>/dev/null)"
  expect "AppRun starts the program and forwards its arguments" "$([[ "$output" == "program 2 one $image" ]]; echo $?)"
  APPIMAGE_EXTRACT_AND_RUN=1 "$image" one two >/dev/null 2>&1
  expect "the program's exit status comes back through the extract-and-run path" "$([ $? = 7 ]; echo $?)"
  # A PATH without fusermount is how a failed mount looks to the runtime; AppRun still needs its own tools.
  mkdir "$work/no-fuse"
  for name in dirname readlink awk grep; do ln -s "$(command -v "$name")" "$work/no-fuse/$name"; done
  output="$(PATH="$work/no-fuse" "$image" fallback 2>/dev/null)"
  expect "the AppImage falls back to extract-and-run when FUSE cannot mount it" "$([[ "$output" == "program 2 fallback $image" ]]; echo $?)"
fi

echo "$checks checks, $failures failures"
[ "$failures" = 0 ]
