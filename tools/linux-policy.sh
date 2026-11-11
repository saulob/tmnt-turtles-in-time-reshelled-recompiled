#!/usr/bin/env bash
# The Linux release policy, shared by tools/package-linux.sh (which stages and checks the real AppDir) and
# tools/test-appimage-release.sh (which runs the same functions on a staged fixture AppDir). Source it after defining
# fail(); nothing runs until a function is called.
#
#   tools/linux-policy.sh --self-test     run the checks against canned readelf output, on any machine with bash
MAX_GLIBC=2.31
BIN_NAME=tmnt_turtles_in_time_reshelled
MODULE_NAME="lib${BIN_NAME}_TMNT_r.so"
APPIMAGETOOL_VERSION=1.9.1
APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0

policy_init() {  # root
  local root="$1"
  APP_ID="$(sed -nE 's/^[[:space:]]*constexpr char kApplicationId\[\] = "([^"]+)";.*/\1/p' "$root/src/platform.cpp" | tr -d '\r')"
  [[ "$APP_ID" =~ ^[A-Za-z0-9_-]+(\.[A-Za-z0-9_-]+)+$ ]] ||
    fail "src/platform.cpp must declare kApplicationId exactly once, as a dotted name on one line (found: '$APP_ID')"
  [ -f "$root/packaging/linux/$APP_ID.desktop" ] || fail "packaging/linux/$APP_ID.desktop is missing: the desktop file is named after kApplicationId"
  # Diagnostics tools/debug-markers.txt ships on purpose (`release`) live in the GPU plugin; nothing else may hold a marker.
  RELEASE_GPU_MARKERS=0
  if tr -d '\r' < "$root/tools/debug-markers.txt" |
    grep -qE '^[[:space:]]*patches/rexglue-sdk/[0-9]{4}-[^[:space:]]+\.linux\.patch[[:space:]]+[0-9]+[[:space:]]+release[[:space:]]*$'; then
    RELEASE_GPU_MARKERS=1
  fi
  EXPECTED="$(LC_ALL=C sort <<EOF
.DirIcon
AppRun
$BIN_NAME.png
$APP_ID.desktop
usr/bin/$BIN_NAME
usr/bin/$BIN_NAME.toml
usr/bin/$MODULE_NAME
usr/bin/assets/launcher/header-art.png
usr/bin/assets/launcher/sponsor-heart.png
usr/bin/librexgpu-xenos.so
usr/bin/librexruntime.so
usr/lib/libgcc_s.so.1
usr/lib/libstdc++.so.6
usr/share/doc/$BIN_NAME/LICENSE
usr/share/doc/$BIN_NAME/ReXGlue-LICENSE.txt
usr/share/doc/$BIN_NAME/THIRD-PARTY-LICENSES-LINUX.txt
usr/share/doc/$BIN_NAME/THIRD-PARTY-LICENSES.txt
usr/share/doc/$BIN_NAME/libstdc++-copyright.txt
usr/share/icons/hicolor/128x128/apps/$BIN_NAME.png
usr/share/icons/hicolor/16x16/apps/$BIN_NAME.png
usr/share/icons/hicolor/24x24/apps/$BIN_NAME.png
usr/share/icons/hicolor/256x256/apps/$BIN_NAME.png
usr/share/icons/hicolor/32x32/apps/$BIN_NAME.png
usr/share/icons/hicolor/48x48/apps/$BIN_NAME.png
usr/share/icons/hicolor/512x512/apps/$BIN_NAME.png
usr/share/icons/hicolor/64x64/apps/$BIN_NAME.png
usr/share/icons/hicolor/scalable/apps/$BIN_NAME.svg
EOF
)"
}

# Checks a staged or extracted AppDir; extra_paths are build or home folders that must not appear in any file.
scan_appdir() {  # appdir label [extra_paths...]
  local appdir="$1" label="$2" item rel base path utf16 f rpath needed actual missing problems=0
  shift 2
  problem() { echo "  $*" >&2; problems=$((problems + 1)); }
  while IFS= read -r -d '' item; do
    rel="${item#"$appdir"/}"
    base="$(basename "$item")"
    case "${base,,}" in
      default.xex | default.xexp | tmnt_r.xex | tmnt.bf | data | game | game-install-temp | game-install-backup | user_data | \
        user_data.migrating | content | cache | logs | settings.toml | settings.toml.bak | arcadeinfo.xml | nxeart | 584109d2 | \
        '$systemupdate' | thumbs.db | desktop.ini | snes | snes_soundtrack | soundset.bin) problem "forbidden name: $rel" ;;
      *.xex | *.xexp | *.bf | *.iso | *.sfc | *.smc | *.swc | *.fig | *.spc | *.rsn | *.wav | *.ogg | *.flac | *.log | *.dmp | *.pdb | *.debug | *.xsh | *.xpso | *.bak | *.tmp | *.sav | *.invalid | \
        *.pyc | *.o | *.obj | *.a | *.lib | *.ilk | *.orig | *.rej | *.rdc | .git* | *~ | settings.toml.*) problem "forbidden file type: $rel" ;;
    esac
    case "${base,,}" in
      *renderdoc* | *vklayer* | tracyclient* | libvulkan* | *.json | implicit_layer.d | explicit_layer.d) problem "RenderDoc, Tracy, Vulkan loader or layer file: $rel" ;;
    esac
    [ -L "$item" ] && problem "symbolic link: $rel"
    if [ -f "$item" ]; then
      case "$(head -c 4 "$item" | tr -d '\0')" in LIVE | PIRS | "CON " | XEX2) problem "Xbox 360 package or executable: $rel" ;; esac
      for path in "$@"; do
        grep -qaF "$path" "$item" && problem "local path in file: $rel ($path)"
        utf16="$(printf '%s' "$path" | od -An -tx1 | tr -d ' \n' | sed 's/../\\x&\\x00/g')"
        LC_ALL=C grep -qaP "$utf16" "$item" && problem "UTF-16 local path in file: $rel ($path)"
      done
      if grep -qaF DEBUG_SAULO "$item" && ! { [ "${RELEASE_GPU_MARKERS:-0}" = 1 ] && [ "$rel" = usr/bin/librexgpu-xenos.so ]; }; then
        problem "DEBUG_SAULO in file: $rel"
      fi
      [ "$(head -c 2 "$item")" = MZ ] && problem "Windows binary: $rel"
    fi
  done < <(find "$appdir" -mindepth 1 -print0)
  actual="$(cd "$appdir" && find . ! -type d | sed 's|^\./||' | LC_ALL=C sort)"
  [ "$actual" = "$EXPECTED" ] || { problem "unexpected AppDir contents:"; diff <(echo "$EXPECTED") <(echo "$actual") >&2 || true; }
  grep -qxF "StartupWMClass=$APP_ID" "$appdir/$APP_ID.desktop" || problem "StartupWMClass in $APP_ID.desktop is not $APP_ID"
  grep -qxF "Exec=$BIN_NAME" "$appdir/$APP_ID.desktop" || problem "Exec in $APP_ID.desktop is not $BIN_NAME"
  grep -qxF "Icon=$BIN_NAME" "$appdir/$APP_ID.desktop" || problem "Icon in $APP_ID.desktop is not $BIN_NAME"
  grep -qaF "$APP_ID" "$appdir/usr/bin/$BIN_NAME" || problem "the program does not contain the application id $APP_ID"
  for f in "$BIN_NAME" "$MODULE_NAME" librexruntime.so librexgpu-xenos.so; do
    if readelf -S -W "$appdir/usr/bin/$f" | grep -F ' .debug_' >/dev/null; then problem "$f holds debug sections"; fi
    rpath="$(readelf -d "$appdir/usr/bin/$f" | elf_rpath_text)"
    needed="$(readelf -d "$appdir/usr/bin/$f" | elf_needed_text | tr '\n' ' ')"
    echo "  $f: RUNPATH [$rpath], needs ${needed:-nothing}"
    case "$f" in "$BIN_NAME") [ "$rpath" = '$ORIGIN' ] || problem "$f RPATH/RUNPATH is '$rpath', not \$ORIGIN" ;; esac
    check_elf_rpath "$f" "$rpath" || problems=$((problems + 1))
    check_elf_needed "$f" "$needed" "$ALLOWED_RUNTIME_NEEDED" || problems=$((problems + 1))
  done
  missing="$(LD_LIBRARY_PATH="$appdir/usr/lib" ldd "$appdir/usr/bin/$BIN_NAME" | grep 'not found' || true)"
  [ -z "$missing" ] || problem "unresolved libraries: $missing"
  [ "$problems" = 0 ] || fail "the $label holds files that must not be released"
  echo "  $label clean"
}

# Copies the files the AppImage holds from a build folder (the program, module, runtime and GPU plugin) and the repository.
stage_appdir() {  # appdir bin_dir symbols_dir source_tree
  local appdir="$1" bin="$2" symbols="$3" tree="$4" doc lib src size icons theme component f
  doc="$appdir/usr/share/doc/$BIN_NAME"
  theme="$appdir/usr/share/icons/hicolor"
  icons="$tree/packaging/icons"
  rm -rf "$appdir" "$symbols"
  mkdir -p "$appdir/usr/bin/assets/launcher" "$appdir/usr/lib" "$doc" "$symbols"
  cp "$bin/$BIN_NAME.toml" "$appdir/usr/bin/"
  cp "$bin/assets/launcher/header-art.png" "$bin/assets/launcher/sponsor-heart.png" "$appdir/usr/bin/assets/launcher/"
  # The debug sections carry the build machine's folders, so they stay in $symbols.
  for f in "$BIN_NAME" "$MODULE_NAME"; do
    objcopy --only-keep-debug "$bin/$f" "$symbols/$f.debug"
    strip --strip-unneeded -o "$appdir/usr/bin/$f" "$bin/$f"
    objcopy --add-gnu-debuglink="$symbols/$f.debug" "$appdir/usr/bin/$f"
  done
  for f in librexruntime.so librexgpu-xenos.so; do
    strip --strip-unneeded -o "$appdir/usr/bin/$f" "$bin/$f"
  done
  # ReXGlue needs GLIBCXX_3.4.32 (GCC 13); AppRun uses this copy only on systems without it.
  for lib in libstdc++.so.6 libgcc_s.so.1; do
    src="$(g++ -print-file-name=$lib 2>/dev/null || true)"
    [ -f "$src" ] || src="/usr/lib/x86_64-linux-gnu/$lib"
    cp -L "$src" "$appdir/usr/lib/$lib"
  done
  grep -q GLIBCXX_3.4.32 "$appdir/usr/lib/libstdc++.so.6" || fail "the build machine's libstdc++ is older than GCC 13"
  cp "$tree/LICENSE" "$tree/packaging/licenses/ReXGlue-LICENSE.txt" "$tree/packaging/licenses/THIRD-PARTY-LICENSES.txt" \
    "$tree/packaging/licenses/THIRD-PARTY-LICENSES-LINUX.txt" "$doc/"
  for component in type2-runtime libfuse squashfuse Zstandard zlib mimalloc musl libgcc; do
    grep -qF "AppImage runtime: $component" "$doc/THIRD-PARTY-LICENSES-LINUX.txt" ||
      fail "THIRD-PARTY-LICENSES-LINUX.txt has no notice for the AppImage runtime component $component"
  done
  [ -f /usr/share/doc/libstdc++6/copyright ] || fail "missing /usr/share/doc/libstdc++6/copyright (the license of the bundled C++ runtime)"
  cp /usr/share/doc/libstdc++6/copyright "$doc/libstdc++-copyright.txt"
  cp "$tree/packaging/linux/AppRun" "$tree/packaging/linux/$APP_ID.desktop" "$appdir/"
  # The icon is the root file the .desktop names, the .DirIcon file managers show, and the theme the desktop integrates.
  for size in 16 24 32 48 64 128 256 512; do
    mkdir -p "$theme/${size}x${size}/apps"
    cp "$icons/app-icon-$size.png" "$theme/${size}x${size}/apps/$BIN_NAME.png"
  done
  mkdir -p "$theme/scalable/apps"
  cp "$icons/app-icon.svg" "$theme/scalable/apps/$BIN_NAME.svg"
  cp "$icons/app-icon-256.png" "$appdir/$BIN_NAME.png"
  cp "$icons/app-icon-256.png" "$appdir/.DirIcon"
  chmod 755 "$appdir/AppRun" "$appdir/usr/bin/$BIN_NAME"
  chmod 644 "$appdir/"*.desktop "$appdir/"*.png "$appdir/.DirIcon" "$appdir/usr/bin/"*.so \
    "$appdir/usr/bin/$BIN_NAME.toml" "$appdir/usr/bin/assets/launcher/header-art.png" \
    "$appdir/usr/bin/assets/launcher/sponsor-heart.png" "$appdir/usr/lib/"* "$doc/"*
  find "$theme" -type f -exec chmod 644 {} +
}

# The shipped runtime and GPU plugin are the built ones with their symbol tables stripped.
holds_built_libraries() {  # bin_dir dir
  local lib stripped
  stripped="$(mktemp)"
  for lib in librexruntime.so librexgpu-xenos.so; do
    strip --strip-unneeded -o "$stripped" "$1/$lib"
    cmp -s "$stripped" "$2/$lib" || { rm -f "$stripped"; return 1; }
  done
  rm -f "$stripped"
}

download() {  # url file sha256
  if [ ! -f "$2" ] || ! echo "$3  $2" | sha256sum -c --status -; then
    rm -f "$2"
    if command -v wget >/dev/null; then wget -q -O "$2" "$1"; else curl -fsSL -o "$2" "$1"; fi
  fi
  echo "$3  $2" | sha256sum -c --status - || fail "$2 does not match its pinned SHA-256"
}

# ---- ELF checks: they read `readelf` output, so canned output can stand in for a binary --------------------------------

elf_needed_text() { sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }    # stdin: readelf -d
elf_rpath_text() { sed -n 's/.*(R\(UN\)\?PATH).*\[\(.*\)\]/\2/p'; }  # stdin: readelf -d

# Every entry must be $ORIGIN-relative: nothing absolute, no developer or build-machine folders.
check_elf_rpath() {  # label rpath_value
  local entry bad=0
  for entry in ${2//:/ }; do
    case "$entry" in
      '$ORIGIN' | '$ORIGIN/'*) ;;
      *) echo "  $1 RPATH/RUNPATH entry '$entry' is not \$ORIGIN-relative" >&2; bad=1 ;;
    esac
  done
  return $bad
}

# Libraries a release file may name. libssl and libcrypto are loaded at run time and must never appear; the X11 and
# Wayland client libraries are what the ReXGlue runtime links, and docs/linux.md lists them as system requirements.
ALLOWED_NEEDED=' libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 libstdc++.so.6 libgcc_s.so.1 ld-linux-x86-64.so.2 librexruntime.so librexgpu-xenos.so '
ALLOWED_RUNTIME_NEEDED=' libX11.so.6 libX11-xcb.so.1 libxcb.so.1 libwayland-client.so.0 '
check_elf_needed() {  # label needed_list extra_allowed
  local lib bad=0
  for lib in $2; do
    case "$lib" in
      libssl* | libcrypto*) echo "  $1 links OpenSSL at build time ($lib); it is loaded at run time" >&2; bad=1 ;;
      *renderdoc* | *RenderDoc* | libVkLayer* | *tracy* | *Tracy*) echo "  $1 depends on a development library ($lib)" >&2; bad=1 ;;
      *) case "$ALLOWED_NEEDED${3:-}" in *" $lib "*) ;; *) echo "  $1 has an unexpected dependency ($lib)" >&2; bad=1 ;; esac ;;
    esac
  done
  return $bad
}

policy_self_test() {
  local failed=0 text root
  expect() {  # description ok|bad command...
    local what="$1" want="$2" got
    shift 2
    "$@" >/dev/null 2>&1
    got=$?
    if { [ "$want" = ok ] && [ "$got" != 0 ]; } || { [ "$want" = bad ] && [ "$got" = 0 ]; }; then
      echo "policy self-test FAILED: $what"
      failed=$((failed + 1))
    fi
  }
  expect '$ORIGIN is allowed' ok check_elf_rpath fixture '$ORIGIN'
  expect '$ORIGIN/../lib is allowed' ok check_elf_rpath fixture '$ORIGIN:$ORIGIN/../lib'
  expect "an empty RUNPATH is allowed" ok check_elf_rpath fixture ''
  expect "an absolute RUNPATH is refused" bad check_elf_rpath fixture "/opt/lib"
  expect "a developer home in RUNPATH is refused" bad check_elf_rpath fixture '$ORIGIN:/home/dev/tmnt/lib'
  expect "a build folder in RUNPATH is refused" bad check_elf_rpath fixture '$ORIGIN:/root/tmnt-linux-build/rexglue/lib'
  expect "a Windows path in RUNPATH is refused" bad check_elf_rpath fixture 'S:/VisualStudio/repos/tmnt/lib'
  expect "the system libraries are allowed" ok check_elf_needed fixture 'libc.so.6 libm.so.6 libstdc++.so.6 librexruntime.so'
  expect "the runtime's display libraries are allowed for the runtime" ok check_elf_needed fixture 'libc.so.6 libX11.so.6' "$ALLOWED_RUNTIME_NEEDED"
  expect "libssl at build time is refused" bad check_elf_needed fixture 'libc.so.6 libssl.so.3'
  expect "libcrypto at build time is refused" bad check_elf_needed fixture 'libc.so.6 libcrypto.so.1.1'
  expect "RenderDoc is refused" bad check_elf_needed fixture 'libc.so.6 librenderdoc.so'
  expect "a Vulkan layer is refused" bad check_elf_needed fixture 'libc.so.6 libVkLayer_khronos_validation.so'
  expect "an unlisted library is refused" bad check_elf_needed fixture 'libc.so.6 libfoo.so.1'
  text='Dynamic section at offset 0x1 contains 3 entries:
 0x0000000000000001 (NEEDED)             Shared library: [libdl.so.2]
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
 0x000000000000001d (RUNPATH)            Library runpath: [$ORIGIN]'
  [ "$(printf '%s\n' "$text" | elf_needed_text | tr '\n' ' ')" = 'libdl.so.2 libc.so.6 ' ] ||
    { echo "policy self-test FAILED: readelf NEEDED parsing"; failed=$((failed + 1)); }
  [ "$(printf '%s\n' "$text" | elf_rpath_text)" = '$ORIGIN' ] ||
    { echo "policy self-test FAILED: readelf RUNPATH parsing"; failed=$((failed + 1)); }
  root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
  policy_init "$root"
  [ "$EXPECTED" = "$(printf '%s\n' "$EXPECTED" | LC_ALL=C sort)" ] ||
    { echo "policy self-test FAILED: the expected AppDir list is not in the order scan_appdir compares"; failed=$((failed + 1)); }
  grep -qxF "$MODULE_NAME" <<<"${EXPECTED//usr\/bin\//}" ||
    { echo "policy self-test FAILED: the expected AppDir list has no guest module"; failed=$((failed + 1)); }
  [ "$failed" = 0 ]
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  fail() { echo "ERROR: $*" >&2; exit 1; }
  [ "${1:-}" = --self-test ] || { echo "usage: $0 --self-test (source it from another script otherwise)" >&2; exit 2; }
  policy_self_test && echo "policy self-test passed"
fi
