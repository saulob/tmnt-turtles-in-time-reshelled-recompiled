#!/usr/bin/env bash
# Builds the Linux x86_64 Release and packages dist/TurtlesInTimeReshelledRecompiled-v<version>-x86_64.AppImage.
# Local only: nothing is published, tagged or uploaded. Run on Linux or WSL.
#
#   tools/package-linux.sh            incremental Release build, then package
#   tools/package-linux.sh --clean    clean Release build first
#   tools/package-linux.sh --verify-only FILE    check a finished AppImage (runtime, contents, glibc) without building
#   tools/package-linux.sh --out-dir DIR    write the AppImage, its .sha256 and the symbols to DIR instead of dist (test builds)
#
# The program's and the module's debug information goes to <out dir>/TurtlesInTimeReshelledRecompiled-v<version>-x86_64-symbols/,
# which is not packaged.
#
# Needs clang-20, clang++-20, lld-20, cmake 3.25+, ninja, rsync, git, wget or curl, unzip or python3, binutils and the
# GCC 13+ C++ runtime. Release builds are made on Ubuntu 20.04 (tools/setup-linux-build-env.sh prepares one): the AppImage
# needs the build machine's glibc.
# The ReXGlue runtime and Vulkan GPU plugin are built from source with the project's patches
# (tools/build-patched-rexruntime.sh) the first time and whenever the patches change, which also needs network
# access and the packages in docs/rexglue-runtime-patch.md.
# The AppImage runtime (the part that mounts the AppImage before AppRun runs) is built from the pinned AppImage/type2-runtime
# source with the project's patch (tools/build-appimage-runtime.sh): that needs root or sudo, network access the first time,
# and is skipped while the pins and patches are unchanged.
# Needs generated/default and generated/tmnt_r (tools/codegen.ps1) and game/extracted/default.xex and TMNT_r.xex: the
# build re-runs codegen, since codegen.d names Windows paths, and it never packages any of them.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# On the Linux filesystem: much faster than a Windows drive under /mnt, and case-sensitive.
WORK="${TMNT_WORK:-$HOME/tmnt-linux-build}"
PRESET=linux-amd64-release

SDK_VERSION=0.10.0
SDK_SHA256=0388edb3f8fb1444f1b0ad5d440cb02d9690ac20a76b04fd6b7dcc19f59328a6
DEFAULT_XEX_SHA256=5ff5b37e120b1f2c3057d78bfc8c8ad6885c3fa576aecb3026e23e6044c5c3de
TMNT_R_XEX_SHA256=c6c19bd1d3de0c0af3651de201e8cac47d9c380c03060734f6f649e83ee89dd9

CLEAN=0
VERIFY_ONLY=
OUT_DIR="$ROOT/dist"
while [ $# -gt 0 ]; do
  case "$1" in
    --clean) CLEAN=1 ;;
    --verify-only) [ $# -ge 2 ] || { echo "--verify-only needs an AppImage" >&2; exit 2; }; VERIFY_ONLY="$(realpath -m "$2")"; shift ;;
    --out-dir) [ $# -ge 2 ] || { echo "--out-dir needs a folder" >&2; exit 2; }; OUT_DIR="$(realpath -m "$2")"; shift ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

fail() { echo "ERROR: $*" >&2; exit 1; }
. "$ROOT/tools/linux-policy.sh"

VERSION="$(sed -nE 's/^[[:space:]]*project\([[:space:]]*tmnt_turtles_in_time_reshelled[[:space:]]+VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$ROOT/CMakeLists.txt")"
[ -n "$VERSION" ] || fail "project VERSION not found in CMakeLists.txt"
policy_init "$ROOT"
NAME="TurtlesInTimeReshelledRecompiled-v$VERSION-x86_64"

if [ -n "$VERIFY_ONLY" ]; then
  [ -f "$VERIFY_ONLY" ] || fail "missing $VERIFY_ONLY"
  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT
  bash "$ROOT/tools/build-appimage-runtime.sh" --check "$VERIFY_ONLY"
  bash "$ROOT/tools/build-appimage-runtime.sh" --check-image "$VERIFY_ONLY"
  cp "$VERIFY_ONLY" "$TMP/verify.AppImage"
  chmod +x "$TMP/verify.AppImage"
  (cd "$TMP" && ./verify.AppImage --appimage-extract >/dev/null)
  scan_appdir "$TMP/squashfs-root" "extracted AppImage" "$ROOT" "$WORK" "$HOME"
  bash "$ROOT/tools/check-glibc-baseline.sh" --max "$MAX_GLIBC" "$TMP/verify.AppImage" "$TMP/squashfs-root" ||
    fail "the AppImage does not meet the glibc $MAX_GLIBC baseline"
  echo "Verified $VERIFY_ONLY"
  exit 0
fi

echo "Turtles in Time Re-Shelled Recompiled $VERSION - Linux AppImage (local only)"
SOURCE="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
if CHANGES="$(git -C "$ROOT" status --porcelain 2>/dev/null)"; then
  [ -z "$CHANGES" ] || SOURCE="$SOURCE plus uncommitted changes"
else
  SOURCE="$SOURCE, working tree state unknown"
fi
echo "Source: $SOURCE"

echo "== Tools"
for tool in clang-20 clang++-20 ld.lld-20 cmake ninja rsync sha256sum objcopy strip readelf objdump ldd; do
  command -v "$tool" >/dev/null || fail "$tool not found"
done
command -v wget >/dev/null || command -v curl >/dev/null || fail "wget or curl not found"
command -v unzip >/dev/null || command -v python3 >/dev/null || fail "unzip or python3 not found"
clang++-20 --version | sed -n 1p
cmake --version | sed -n 1p
HOST_GLIBC="$(getconf GNU_LIBC_VERSION | cut -d' ' -f2)"
echo "glibc $HOST_GLIBC"
if ! grep -q '^VERSION_ID="20.04"' /etc/os-release || [ "$HOST_GLIBC" != "$MAX_GLIBC" ]; then
  [ "${TMNT_ALLOW_UNSUPPORTED_HOST:-0}" = 1 ] ||
    fail "release builds need Ubuntu 20.04 (glibc $MAX_GLIBC), see docs/linux.md; TMNT_ALLOW_UNSUPPORTED_HOST=1 skips this check but not the glibc check on the files"
  echo "WARNING: unsupported build host" >&2
fi
bash "$ROOT/tools/check-glibc-baseline.sh" --self-test
policy_self_test || fail "the release policy self-test failed"

echo "== Inputs"
[ -f "$ROOT/generated/default/sources.cmake" ] || fail "generated/default is missing (run tools/codegen.ps1 first)"
[ -f "$ROOT/third_party/trg-launcher/CMakeLists.txt" ] || fail "run: git submodule update --init third_party/trg-launcher"
[ -f "$ROOT/generated/tmnt_r/sources.cmake" ] || fail "generated/tmnt_r is missing (run tools/codegen.ps1 first)"
check_xex() {  # name sha256
  [ -f "$ROOT/game/extracted/$1" ] || fail "missing game/extracted/$1 (extract your own XBLA package into game/extracted first)"
  echo "$2  $ROOT/game/extracted/$1" | sha256sum -c --status - || fail "unsupported game/extracted/$1"
}
check_xex default.xex "$DEFAULT_XEX_SHA256"
check_xex TMNT_r.xex "$TMNT_R_XEX_SHA256"

echo "== Source -> $WORK"
mkdir -p "$WORK/game/extracted" "$WORK/third_party" "$WORK/tools"
for f in CMakeLists.txt CMakePresets.json tmnt_turtles_in_time_reshelled_manifest.toml LICENSE README.md tools/build-patched-rexruntime.sh tools/package-release.ps1 tools/package-linux.sh; do
  rsync -a "$ROOT/$f" "$WORK/$f"
done
for d in assets config docs src tests generated packaging patches third_party/trg-launcher; do
  mkdir -p "$WORK/$d"
  rsync -a --delete --exclude .git "$ROOT/$d/" "$WORK/$d/"
done
# A Windows checkout has CRLF sources; the TRG patches are applied with git apply and need matching endings.
find "$WORK/third_party/trg-launcher" "$WORK/patches/trg-launcher" -type f \
  \( -name '*.patch' -o -name '*.h' -o -name '*.cpp' -o -name '*.c' -o -name '*.txt' -o -name '*.cmake' \) \
  -exec sed -i 's/\r$//' {} +
rsync -a "$ROOT/game/extracted/default.xex" "$ROOT/game/extracted/TMNT_r.xex" "$WORK/game/extracted/"

echo "== ReXGlue SDK $SDK_VERSION (linux-amd64)"
SDK="$WORK/tools/rexglue/linux-amd64"
if [ ! -x "$SDK/bin/rexglue" ]; then
  mkdir -p "$WORK/tools/rexglue"
  ZIP="$WORK/tools/rexglue/rexglue-sdk-$SDK_VERSION-linux-amd64.zip"
  download "https://github.com/rexglue/rexglue-sdk/releases/download/v$SDK_VERSION/rexglue-sdk-$SDK_VERSION-linux-amd64.zip" \
    "$ZIP" "$SDK_SHA256"
  rm -rf "$SDK"
  if command -v unzip >/dev/null; then
    unzip -q -o "$ZIP" -d "$WORK/tools/rexglue"
  else
    python3 -I -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$ZIP" "$WORK/tools/rexglue"
    chmod +x "$SDK/bin/rexglue"
  fi
fi
# The stock rexglue tool needs a newer glibc than the build baseline, so the version is read from the CMake package.
grep -qF "set(PACKAGE_VERSION \"$SDK_VERSION\")" "$SDK/lib/cmake/rexglue/rexglueConfigVersion.cmake" ||
  fail "tools/rexglue/linux-amd64 is not ReXGlue $SDK_VERSION"

echo "== Patched ReXGlue runtime and GPU plugin"
PATCHED="$ROOT/tools/build-patched-rexruntime.sh"
if ! bash "$PATCHED" --check "$SDK" >/dev/null 2>&1; then
  bash "$PATCHED" "$SDK" "$WORK/rexglue-sdk-src"
fi
bash "$PATCHED" --check "$SDK"

echo "== Patched rexglue code generator"
bash "$PATCHED" --install-generator "$SDK" "$WORK/rexglue-sdk-src"

echo "== Build ($PRESET)"
BIN="$WORK/out/build/$PRESET"
[ "$CLEAN" = 1 ] && rm -rf "$BIN"
(cd "$WORK" && cmake --preset "$PRESET" >/dev/null && cmake --build --preset "$PRESET")
for f in "$BIN_NAME" "$MODULE_NAME" librexruntime.so librexgpu-xenos.so "$BIN_NAME.toml" assets/launcher/header-art.png assets/launcher/sponsor-heart.png; do
  [ -f "$BIN/$f" ] || fail "build output is missing $f"
done
bash "$PATCHED" --check "$SDK" --staged "$BIN" || fail "the build folder does not hold the patched ReXGlue libraries"

echo "== AppDir"
APPDIR="$WORK/AppDir"
SYMBOLS="$OUT_DIR/$NAME-symbols"
stage_appdir "$APPDIR" "$BIN" "$SYMBOLS" "$WORK"
holds_built_libraries "$BIN" "$APPDIR/usr/bin" || fail "the AppDir does not hold the patched ReXGlue libraries"

echo "== Release check"
scan_appdir "$APPDIR" "AppDir" "$ROOT" "$WORK" "$HOME"

echo "== glibc $MAX_GLIBC baseline (AppDir)"
bash "$ROOT/tools/check-glibc-baseline.sh" --max "$MAX_GLIBC" "$APPDIR" || fail "the AppDir does not meet the glibc $MAX_GLIBC baseline"

echo "== AppImage"
TOOLS="$WORK/tools/appimage"
mkdir -p "$TOOLS"
TOOL="$TOOLS/appimagetool-$APPIMAGETOOL_VERSION-x86_64.AppImage"
RUNTIME="$TOOLS/runtime-tmnt-x86_64"
download "https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL_VERSION/appimagetool-x86_64.AppImage" \
  "$TOOL" "$APPIMAGETOOL_SHA256"
bash "$ROOT/tools/build-appimage-runtime.sh" "$RUNTIME" "$WORK/appimage-runtime"
chmod +x "$TOOL"
mkdir -p "$OUT_DIR"
OUT="$OUT_DIR/$NAME.AppImage"
rm -f "$OUT"
ARCH=x86_64 "$TOOL" --appimage-extract-and-run --no-appstream --runtime-file "$RUNTIME" "$APPDIR" "$OUT" >/dev/null
chmod 755 "$OUT"
bash "$ROOT/tools/build-appimage-runtime.sh" --check "$OUT"
bash "$ROOT/tools/build-appimage-runtime.sh" --check-image "$OUT"

echo "== glibc $MAX_GLIBC baseline (AppImage)"
CHECK="$(mktemp -d)"
trap 'rm -rf "$CHECK"' EXIT
(cd "$CHECK" && "$OUT" --appimage-extract >/dev/null)
scan_appdir "$CHECK/squashfs-root" "extracted AppImage" "$ROOT" "$WORK" "$HOME" || { rm -f "$OUT"; exit 1; }
holds_built_libraries "$BIN" "$CHECK/squashfs-root/usr/bin" || { rm -f "$OUT"; fail "the AppImage does not hold the built runtime"; }
bash "$ROOT/tools/check-glibc-baseline.sh" --max "$MAX_GLIBC" "$OUT" "$CHECK/squashfs-root" ||
  { rm -f "$OUT"; fail "the AppImage does not meet the glibc $MAX_GLIBC baseline"; }
(cd "$OUT_DIR" && sha256sum "$NAME.AppImage" > "$NAME.sha256")

echo
echo "===================================================="
echo " APPIMAGE READY (nothing was published)"
echo "===================================================="
echo "  AppImage: $OUT ($(du -h "$OUT" | cut -f1))"
echo "  SHA-256:  $(cut -d' ' -f1 "$OUT_DIR/$NAME.sha256")"
echo "  Symbols:  $SYMBOLS (not in the AppImage)"
echo "  Source:   $SOURCE"
echo "  Contents:"
echo "$EXPECTED" | sed 's/^/    /'
