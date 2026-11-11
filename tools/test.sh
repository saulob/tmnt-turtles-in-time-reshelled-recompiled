#!/usr/bin/env bash
# Headless Windows/Linux parity gate for Linux. It reads the same group list, feature manifest and temporary-marker
# inventory as tools/test.ps1, runs tools/parity_gate.py, and adds the checks that need a shell or a compiler.
# Nothing is launched, downloaded or published. Exit 1 on a real failure; NOT RUN is reported, never turned into PASS.
#
#   tools/test.sh
#   tools/test.sh --release
#   tools/test.sh --cpp-tests build-linux

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
RESULTS="$(mktemp)"
trap 'rm -f "$RESULTS"' EXIT
RELEASE=0
CPP_TESTS=""

while [ $# -gt 0 ]; do
  case "$1" in
    --release) RELEASE=1 ;;
    --cpp-tests) CPP_TESTS="${2:-}"; shift ;;
    --cpp-tests=*) CPP_TESTS="${1#*=}" ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

record() { printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$(printf '%s' "${4:-}" | tr '\n\t' '  ')" >> "$RESULTS"; }

run_check() {
  local group="$1" name="$2"
  shift 2
  local output
  if output="$("$@" 2>&1)"; then
    record "$group" "$name" PASS ""
  else
    record "$group" "$name" FAIL "$(printf '%s\n' "$output" | tail -n 3 | tr '\n' ' ')"
  fi
}

have() { command -v "$1" >/dev/null 2>&1; }

compiler_works() {
  local work
  work="$(mktemp -d)"
  printf 'int main() { return 0; }\n' > "$work/t.cpp"
  if timeout 60 "$1" -std=c++17 "$work/t.cpp" -o "$work/t" >/dev/null 2>&1 && [ -x "$work/t" ]; then
    rm -rf "$work"
    return 0
  fi
  rm -rf "$work"
  return 1
}

groups=()
while IFS= read -r line; do
  line="${line%$'\r'}"
  [ -z "${line//[[:space:]]/}" ] && continue
  case "$line" in \#*) continue ;; esac
  groups+=("$line")
done < tools/test-groups.txt

# ---- Shared manifest gate ---------------------------------------------------------------------------------------------
PYTHON=""
for candidate in python3 python; do
  if have "$candidate"; then PYTHON="$candidate"; break; fi
done
if [ -n "$PYTHON" ]; then
  gate_output="$("$PYTHON" tools/parity_gate.py --root "$ROOT" 2>&1)"
  gate_code=$?
  while IFS=$'\t' read -r tag group name status detail; do
    [ "$tag" = "CHECK" ] || continue
    record "$group" "$name" "$status" "${detail:-}"
  done <<< "$gate_output"
  if ! printf '%s\n' "$gate_output" | grep -q '^CHECK'; then
    record "Cross-platform parity" "shared Windows/Linux feature matrix" FAIL "parity_gate.py exited $gate_code without results"
  fi
else
  record "Cross-platform parity" "shared Windows/Linux feature matrix" "NOT RUN" "python is not installed"
fi

# ---- Core -------------------------------------------------------------------------------------------------------------
shell_failures=""
for script in tools/*.sh packaging/linux/AppRun; do
  [ -f "$script" ] || continue
  if ! bash -n "$script" >/tmp/tmnt-bash-n.log 2>&1; then
    shell_failures="$shell_failures $script"
  fi
done
if [ -z "$shell_failures" ]; then
  record Core "shell scripts parse" PASS ""
else
  record Core "shell scripts parse" FAIL "$shell_failures"
fi

if have git; then
  run_check Core "git diff --check" git diff --check -- . ':(exclude)tools/xex-dump/**' ':(exclude)third_party/**' ':(exclude)tools/rexglue/**'
else
  record Core "git diff --check" "NOT RUN" "git is not installed"
fi

version="$(sed -n 's/^project(tmnt_turtles_in_time_reshelled VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt | head -n 1)"
if [ -n "$version" ] && grep -q 'TMNT_EXPECTED_PORT_VERSION' CMakeLists.txt; then
  record Core "one project version" PASS ""
else
  record Core "one project version" FAIL "CMakeLists.txt has no project VERSION or TMNT_PORT_VERSION"
fi

if [ -n "$CPP_TESTS" ] && [ -f "$CPP_TESTS/CMakeCache.txt" ]; then
  if cmake --build "$CPP_TESTS" --target tmnt_runtime_tests >/tmp/tmnt-cpp-build.log 2>&1; then
    failures=""
    count=0
    fl_cpp_missing="tmnt_online_protocol_tests tmnt_online_service_tests tmnt_online_identity_tests tmnt_online_setting_tests tmnt_online_telemetry_tests tmnt_platform_parity_tests tmnt_incident_tests"
    fl_cpp_failures=""
    # A build folder can keep a test binary from another branch; run only targets this configuration defines.
    known_targets="$(cmake --build "$CPP_TESTS" --target help 2>/dev/null |
      sed -n 's/^[[:space:]]*\(tmnt_[A-Za-z0-9_]*_tests\)[[:space:]]*:.*/\1/p')"
    while IFS= read -r program; do
      base="$(basename "$program" .exe)"
      case "$base" in tmnt_shutdown_order_tests*) continue ;; esac
      if [ -n "$known_targets" ] && ! printf '%s\n' "$known_targets" | grep -qx "$base"; then continue; fi
      count=$((count + 1))
      "$program" >/dev/null 2>&1
      code=$?
      if [ "$code" -ne 0 ] && [ "$code" -ne 77 ]; then failures="$failures $base($code)"; fi
      case " $fl_cpp_missing " in *" $base "*)
        fl_cpp_missing="${fl_cpp_missing/$base/}"
        [ "$code" -ne 0 ] && fl_cpp_failures="$fl_cpp_failures $base($code)" ;;
      esac
    done < <(find "$CPP_TESTS" -maxdepth 1 -type f \( -name 'tmnt_*_tests' -o -name 'tmnt_*_tests.exe' \) | sort)
    for missing in $fl_cpp_missing; do fl_cpp_failures="$fl_cpp_failures $missing(not built)"; done
    if [ -n "${fl_cpp_failures// /}" ]; then
      record "Friends / Lobby" "client: protocol, service, identity, setting, telemetry, platform parity and Bug Report test programs" FAIL "$fl_cpp_failures"
    else
      record "Friends / Lobby" "client: protocol, service, identity, setting, telemetry, platform parity and Bug Report test programs" PASS ""
    fi
    if [ -n "$failures" ]; then
      record Core "C++ tests ($count programs)" FAIL "$failures"
    else
      record Core "C++ tests ($count programs)" PASS ""
    fi
  else
    record Core "C++ tests" FAIL "cmake --build --target tmnt_runtime_tests failed"
  fi
else
  record Core "C++ tests" "NOT RUN" "pass --cpp-tests <a configured build folder> to build and run them"
  record "Friends / Lobby" "client: protocol, service, identity, setting, telemetry, platform parity and Bug Report test programs" "NOT RUN" "pass --cpp-tests <a configured build folder>"
fi

# ---- Cross-platform parity: the same C++ static test the Windows gate builds -----------------------------------------
COMPILER=""
for candidate in clang++-20 clang++ g++; do
  if have "$candidate" && compiler_works "$candidate"; then COMPILER="$candidate"; break; fi
done
if [ -n "$COMPILER" ]; then
  work="$(mktemp -d)"
  if timeout 300 "$COMPILER" -std=c++23 -O1 -w "-DTMNT_ROOT_DIR=\"$ROOT\"" tests/platform_parity_tests.cpp -o "$work/parity" >"$work/log" 2>&1; then
    if "$work/parity" >"$work/run" 2>&1; then
      record "Cross-platform parity" "C++ feature-matrix test" PASS ""
    else
      record "Cross-platform parity" "C++ feature-matrix test" FAIL "$(tail -n 3 "$work/run" | tr '\n' ' ')"
    fi
  else
    record "Cross-platform parity" "C++ feature-matrix test" FAIL "$(tail -n 3 "$work/log" | tr '\n' ' ')"
  fi
  rm -rf "$work"
else
  record "Cross-platform parity" "C++ feature-matrix test" "NOT RUN" "no working clang++ or g++ found"
fi

# ---- Focused portable tests (the same sources tools/test.ps1 compiles) ------------------------------------------------
focused_test() {
  local group="$1" name="$2" source="$3"
  shift 3
  if [ -z "$COMPILER" ]; then
    record "$group" "$name" "NOT RUN" "no working C++ compiler"
    return
  fi
  local work
  work="$(mktemp -d)"
  local args=(-std=c++23 -O1 -w "-DTMNT_ROOT_DIR=\"$ROOT\"" "-I$ROOT/src")
  args+=("$@")
  if timeout 300 "$COMPILER" "${args[@]}" "$source" -o "$work/test" >"$work/build.log" 2>&1; then
    if timeout 180 "$work/test" >"$work/run.log" 2>&1; then
      record "$group" "$name" PASS ""
    else
      record "$group" "$name" FAIL "$(tail -n 3 "$work/run.log" | tr '\n' ' ')"
    fi
  else
    record "$group" "$name" FAIL "compile failed: $(tail -n 2 "$work/build.log" | tr '\n' ' ')"
  fi
  rm -rf "$work"
}

focused_test Gameplay "Title music level: no volume change at Press START" "$ROOT/tests/title_bgm_level_tests.cpp"
focused_test Gameplay "Skip intro: setting, boot path Off and On, game image" "$ROOT/tests/skip_intro_tests.cpp"
focused_test Gameplay "Menu cleanup: removed Xbox rows and footers" "$ROOT/tests/fire_movie_patch_tests.cpp"
focused_test Graphics "Aspect ratio: 16:9 only, old values normalized" "$ROOT/tests/aspect_ratio_tests.cpp" "-DTMNT_SRC_DIR=\"$ROOT/src\""
focused_test Graphics "Internal render resolution removed" "$ROOT/tests/internal_render_removed_tests.cpp"
trg_sources=("-I$ROOT/third_party/trg-launcher/include" "$ROOT/third_party/trg-launcher/src/settings.cpp"
  "$ROOT/third_party/trg-launcher/src/platform.cpp")
if [ "$(uname -s)" != "Linux" ]; then
  trg_sources+=(-luser32 -lshell32 -lole32 -lcomdlg32)
fi
focused_test Core "Detailed logging: normal and Detailed-only lines" "$ROOT/tests/log_policy_tests.cpp" "${trg_sources[@]}"
focused_test Cheats "Cheats: rows, session overrides, online suppression" "$ROOT/tests/cheats_tests.cpp" "${trg_sources[@]}"
focused_test Localization "Portugues Brasil: registration, tables and font" "$ROOT/tests/localization_tests.cpp"
focused_test Core "Audio teardown regression (patch 0086)" "$ROOT/tests/audio_client_teardown_tests.cpp" -pthread \
  "-DTMNT_PATCH_DIR=\"$ROOT/patches/rexglue-sdk\""

sdk_include="$ROOT/tools/rexglue/linux-amd64/include"
if [ -f "$sdk_include/imgui.h" ] && [ -d "$ROOT/third_party/trg-launcher/include" ]; then
  focused_test Controls "Host keys: F3 Ping and removed SDK overlays" "$ROOT/tests/host_key_tests.cpp" \
    "-I$sdk_include" "-I$ROOT/third_party/trg-launcher/include"
else
  record Controls "Host keys: F3 Ping and removed SDK overlays" "NOT RUN" "$sdk_include/imgui.h is missing"
fi

sdk_source="${REXGLUE_SDK_REPO:-$ROOT/out/rexglue-sdk-src}"
if [ -f "$sdk_include/rex/system/achievement_manager.h" ] && [ -f "$sdk_source/src/system/kernel_state.cpp" ]; then
  focused_test Achievements "Local profile: controller START owns it" "$ROOT/tests/local_profile_owner_tests.cpp" \
    "-I$sdk_include" "-DTMNT_SRC_DIR=\"$ROOT/src\"" "-DTMNT_SDK_SOURCE_DIR=\"$sdk_source\""
else
  record Achievements "Local profile: controller START owns it" "NOT RUN" "the pinned ReXGlue SDK source is missing"
fi

# ---- Online and Worker ------------------------------------------------------------------------------------------------
friends_lobby_files=()
while IFS= read -r line; do
  line="${line%$''}"
  case "$line" in ''|\#*) continue ;; esac
  friends_lobby_files+=("$line")
done < tools/friends-lobby-tests.txt

if have node; then
  worker_files=()
  for file in server/tmnt-lobby/test/*.test.js; do
    name="$(basename "$file")"
    skip=0
    for listed in "${friends_lobby_files[@]}"; do [ "$listed" = "$name" ] && skip=1; done
    [ "$skip" -eq 0 ] && worker_files+=("test/$name")
  done
  if (cd server/tmnt-lobby && timeout 120 node --test "${worker_files[@]}" >/tmp/tmnt-worker-tests.log 2>&1); then
    record Worker "Worker tests (node --test), without the Friends / Lobby files" PASS ""
  else
    record Worker "Worker tests (node --test), without the Friends / Lobby files" FAIL "$(tail -n 3 /tmp/tmnt-worker-tests.log | tr '
' ' ')"
  fi
  lobby_args=()
  for listed in "${friends_lobby_files[@]}"; do lobby_args+=("test/$listed"); done
  if (cd server/tmnt-lobby && timeout 120 node --test "${lobby_args[@]}" >/tmp/tmnt-friends-lobby-tests.log 2>&1); then
    record "Friends / Lobby" "Worker: aliases, handles, request flow, privacy before acceptance, Lobby and Bug Report" PASS ""
  else
    record "Friends / Lobby" "Worker: aliases, handles, request flow, privacy before acceptance, Lobby and Bug Report" FAIL       "$(grep -E 'leaked|did not|alias|handle|Pending|request' /tmp/tmnt-friends-lobby-tests.log | head -n 3 | tr '
' ' ')"
  fi
else
  record Worker "Worker tests (node --test)" "NOT RUN" "node is not installed"
  record "Friends / Lobby" "Worker: aliases, handles, request flow, privacy before acceptance, Lobby and Bug Report" "NOT RUN" "node is not installed"
fi

fl_problems=""
worker_build="$(sed -nE "s/.*WORKER_BUILD = '([^']+)'.*/\1/p" server/tmnt-lobby/src/protocol.js | tr -d '\r' | head -n 1)"
pinned_build="$(sed -nE "s/.*WORKER_BUILD, '([^']+)'.*/\1/p" server/tmnt-lobby/test/lobby.test.js | tr -d '\r' | head -n 1)"
[ -n "$worker_build" ] && [ "$worker_build" = "$pinned_build" ] || fl_problems="$fl_problems lobby.test.js pins '$pinned_build' but protocol.js says '$worker_build';"
while IFS= read -r other; do
  [ "$other" = "$worker_build" ] || fl_problems="$fl_problems a source names worker build $other;"
done < <(git ls-files -z -- server/tmnt-lobby/src server/tmnt-lobby/local server/tmnt-lobby/tools docs src |
  xargs -0 grep -ohE '\b20[0-9]{2}-[0-9]{2}-[0-9]{2}-(lobby|party)[0-9]+\b' 2>/dev/null | sort -u)
client_protocol="$(sed -nE 's/.*kLobbyProtocol = ([0-9]+).*/\1/p' src/online/protocol.h | tr -d '\r')"
worker_protocol="$(sed -nE 's/.*export const PROTOCOL = ([0-9]+).*/\1/p' server/tmnt-lobby/src/protocol.js | tr -d '\r')"
[ "$client_protocol" = "$worker_protocol" ] && [ "$worker_protocol" = 4 ] || fl_problems="$fl_problems lobby protocol is client '$client_protocol', Worker '$worker_protocol', and must stay 4;"
if [ -z "$fl_problems" ]; then
  record "Friends / Lobby" "contract: one Worker build constant, pinned by its test; protocol 4 on both sides" PASS ""
else
  record "Friends / Lobby" "contract: one Worker build constant, pinned by its test; protocol 4 on both sides" FAIL "$fl_problems"
fi

offline=""
for listed in "${friends_lobby_files[@]}"; do
  grep -qiE 'workers\.dev|(https?|wss?)://[a-z0-9-]+\.[a-z]' "server/tmnt-lobby/test/$listed" && offline="$offline $listed"
done
if [ -z "$offline" ]; then
  record "Friends / Lobby" "offline: the gate tests never name the production Worker or any external host" PASS ""
else
  record "Friends / Lobby" "offline: the gate tests never name the production Worker or any external host" FAIL "names a host:$offline"
fi

if grep -nE '\b(player|sender|r|request)\.(player|id)\b' src/online_page.cpp | grep -E 'Text|ui\.Row|ui\.Info|Label|Notice' >/tmp/tmnt-ui-identity.txt; then
  record "Friends / Lobby" "UI: the Lobby page never renders a handle or a friend code as text" FAIL "$(head -n 2 /tmp/tmnt-ui-identity.txt | tr '
' ' ')"
else
  record "Friends / Lobby" "UI: the Lobby page never renders a handle or a friend code as text" PASS ""
fi

if [ -f server/tmnt-lobby/local/server.js ] && [ -f server/tmnt-lobby/wrangler.toml ]; then
  record Online "local test lobby and Worker configuration exist" PASS ""
else
  record Online "local test lobby and Worker configuration exist" FAIL "server/tmnt-lobby is incomplete"
fi

# ---- Linux contracts --------------------------------------------------------------------------------------------------
linux_contract() {
  local name="$1" path="$2" needle="$3"
  if [ ! -f "$path" ]; then
    record "Linux contracts" "$name" FAIL "$path is missing"
  elif grep -Fq -- "$needle" "$path"; then
    record "Linux contracts" "$name" PASS ""
  else
    record "Linux contracts" "$name" FAIL "$path lacks $needle"
  fi
}
linux_contract "user_data follows the AppImage" src/platform.cpp 'getenv("APPIMAGE")'
linux_contract "logs live in user_data" src/log_path.cpp 'user_data_root / "logs"'
linux_contract "relaunch restarts the AppImage" src/platform.cpp posix_spawn
linux_contract "crash handlers are installed" src/crash_report.cpp InstallPosixHandlers
linux_contract "the updater replaces the AppImage" src/updater/update_platform.cpp ApplyAppImageUpdate
linux_contract "Bug Report says Linux" src/incident_report.cpp '\"platform\":\"Linux\"'
linux_contract "the launcher has every page" src/launcher.cpp 'LauncherPage::kBugReport'
linux_contract "settings live in user_data" src/settings.h 'user_data_root / "settings.toml"'
linux_contract "online identity lives in user_data" src/online/identity.cpp kIdentityFileName
linux_contract "achievements are read from the game data" src/launcher_achievements.cpp ListAchievements
linux_contract "XDG folders are private" src/log_scrubber.cpp XDG_DATA_HOME
linux_contract "the Linux program uses an ORIGIN RPATH" CMakeLists.txt 'RPATH is $ORIGIN'

expect_needles() {
  local name="$1" path="$2"
  shift 2
  local missing=""
  if [ ! -f "$path" ]; then
    record "Linux contracts" "$name" FAIL "$path is missing"
    return
  fi
  for needle in "$@"; do
    grep -Fq -- "$needle" "$path" || missing="$missing $needle"
  done
  if [ -z "$missing" ]; then
    record "Linux contracts" "$name" PASS ""
  else
    record "Linux contracts" "$name" FAIL "$path lacks:$missing"
  fi
}

expect_needles "Linux CMake presets" CMakePresets.json linux-amd64-release linux-amd64-relwithdebinfo linux-amd64-debug clang-20 clang++-20
expect_needles "AppRun starts the program and picks the C++ runtime" packaging/linux/AppRun \
  'exec "$HERE/usr/bin/tmnt_turtles_in_time_reshelled" "$@"' GLIBCXX_3.4.32

app_id="$(sed -n 's/.*constexpr char kApplicationId\[\] = "\([^"]*\)";.*/\1/p' src/platform.cpp | head -n 1)"
if [ -n "$app_id" ] && [ -f "packaging/linux/$app_id.desktop" ]; then
  expect_needles "the desktop file matches the application id" "packaging/linux/$app_id.desktop" \
    '[Desktop Entry]' 'Type=Application' 'Icon=tmnt_turtles_in_time_reshelled' "StartupWMClass=$app_id"
else
  record "Linux contracts" "the desktop file matches the application id" FAIL "src/platform.cpp has no kApplicationId or the .desktop file is missing"
fi

missing_icons=""
for size in 16 24 32 48 64 128 256 512; do
  [ -f "packaging/icons/app-icon-$size.png" ] || missing_icons="$missing_icons $size"
done
if [ -z "$missing_icons" ]; then
  record "Linux contracts" "the AppImage icons are present" PASS ""
else
  record "Linux contracts" "the AppImage icons are present" FAIL "missing:$missing_icons"
fi

missing_licenses=""
for license in packaging/licenses/ReXGlue-LICENSE.txt packaging/licenses/THIRD-PARTY-LICENSES.txt \
  packaging/licenses/THIRD-PARTY-LICENSES-LINUX.txt LICENSE; do
  [ -s "$license" ] || missing_licenses="$missing_licenses $license"
done
if [ -z "$missing_licenses" ]; then
  record "Linux contracts" "the AppImage licenses are present" PASS ""
else
  record "Linux contracts" "the AppImage licenses are present" FAIL "missing or empty:$missing_licenses"
fi

policy_text="$(mktemp)"
cat tools/package-linux.sh tools/linux-policy.sh > "$policy_text"
expect_needles "AppImage packaging refuses data and local paths" "$policy_text" \
  'MAX_GLIBC=2.31' 'VERSION_ID="20.04"' check-glibc-baseline.sh appimagetool APPIMAGETOOL_SHA256 \
  build-appimage-runtime.sh sha256sum 'forbidden name' 'Xbox 360 package' 'local path in file' 'Windows binary' \
  'Vulkan loader or layer file' 'GLIBCXX_3.4.32' --verify-only 'scan_appdir "$CHECK/squashfs-root"' \
  --check-image holds_built_libraries 'objcopy --only-keep-debug' '$NAME-symbols'
rm -f "$policy_text"

expect_needles "the AppImage runtime is pinned" patches/appimage-runtime/baseline.env RUNTIME_SHA256 RUNTIME_COMMIT
expect_needles "the AppImage runtime keeps the FUSE fallback" patches/appimage-runtime/0001-extract-and-run-when-fuse-fails.patch extract fuse

if bash tools/check-glibc-baseline.sh --self-test >/dev/null 2>&1; then
  record "Linux contracts" "check-glibc-baseline self-test" PASS ""
else
  record "Linux contracts" "check-glibc-baseline self-test" FAIL "tools/check-glibc-baseline.sh --self-test failed"
fi
if bash tools/linux-policy.sh --self-test >/dev/null 2>&1; then
  record "Linux contracts" "linux-policy self-test" PASS ""
else
  record "Linux contracts" "linux-policy self-test" FAIL "tools/linux-policy.sh --self-test failed"
fi

if [ -f tools/test-appimage-release.sh ]; then
  if output="$(timeout 600 bash tools/test-appimage-release.sh "$ROOT" 2>&1)"; then
    detail="$(printf '%s\n' "$output" | grep '^NOT RUN:' | tail -n 1 || true)"
    record "Linux contracts" "AppImage release checks" PASS "$detail"
  else
    record "Linux contracts" "AppImage release checks" FAIL \
      "$(printf '%s\n' "$output" | grep -E '^(FAIL|ERROR)' | tail -n 3 | tr '\n' ' ')"
  fi
else
  record "Linux contracts" "AppImage release checks" "NOT RUN" "tools/test-appimage-release.sh is missing"
fi

# ---- Hygiene ----------------------------------------------------------------------------------------------------------
if have git; then
  forbidden="$(git ls-files --cached --others --exclude-standard | grep -E '(^|/)(default\.xex|TMNT_r\.xex|TMNT\.bf|build_log\.txt)$|(^|/)(user_data|user_data\.migrating|logs|game|dist|build|build-release|scratchpad|snes|snes_soundtrack)/|(^|/)soundset\.bin$|\.(log|dmp|pdb|sav|xsh|xpso|bak|tmp|exe|dll|so|AppImage|sfc|smc|swc|fig|spc|rsn|wav|ogg|flac)$' || true)"
  if [ -z "$forbidden" ]; then
    record Hygiene "no game data, logs, saves, player data or build output" PASS ""
  else
    record Hygiene "no game data, logs, saves, player data or build output" FAIL "$(printf '%s\n' "$forbidden" | head -n 3 | tr '\n' ' ')"
  fi

  declare -A marker_allowed=()
  declare -A marker_release=()
  if [ -f tools/debug-markers.txt ]; then
    while read -r path count ship; do
      path="${path%$'\r'}"
      [ -z "$path" ] && continue
      case "$path" in \#*) continue ;; esac
      marker_allowed["$path"]="${count%$'\r'}"
      [ "${ship%$'\r'}" = release ] && marker_release["$path"]=1
    done < tools/debug-markers.txt
  fi
  marker_failures=""
  while IFS= read -r file; do
    path="${file#./}"
    actual="$(grep -o 'DEBUG_SAULO' "$file" | wc -l | tr -d ' ')"
    if [ "$RELEASE" -eq 1 ] && [ -z "${marker_release[$path]:-}" ]; then
      marker_failures="$marker_failures $path"
    elif [ -z "${marker_allowed[$path]:-}" ]; then
      marker_failures="$marker_failures $path"
    elif [ "$actual" -ne "${marker_allowed[$path]}" ]; then
      marker_failures="$marker_failures $path($actual!=${marker_allowed[$path]})"
    fi
  done < <(git ls-files -z --cached --others --exclude-standard -- src tools docs server patches |
    xargs -0 grep -l 'DEBUG_SAULO' 2>/dev/null |
    grep -v -E '^(docs/|tests/|tools/(test\.ps1|test\.sh|debug-markers\.txt|package-release\.ps1|linux-policy\.sh|test-appimage-release\.sh|test-release-check\.ps1))' || true)
  if [ "${#marker_allowed[@]}" -gt 0 ]; then
    for path in "${!marker_allowed[@]}"; do
      [ -f "$path" ] || marker_failures="$marker_failures $path(missing)"
    done
  fi
  if [ -z "$marker_failures" ]; then
    record Hygiene "DEBUG_SAULO inventory matches tools/debug-markers.txt" PASS ""
  else
    record Hygiene "DEBUG_SAULO inventory matches tools/debug-markers.txt" FAIL "$marker_failures"
  fi
else
  record Hygiene "repository hygiene" "NOT RUN" "git is not installed"
fi

missing_ignore=""
for entry in '/game/' 'user_data/' '*.log' '/build/' '/out/' '/dist/' '/tools/rexglue/'; do
  grep -Fq -- "$entry" .gitignore || missing_ignore="$missing_ignore $entry"
done
if [ -z "$missing_ignore" ]; then
  record Hygiene ".gitignore keeps game data, player data, logs and build output out" PASS ""
else
  record Hygiene ".gitignore keeps game data, player data, logs and build output out" FAIL "missing:$missing_ignore"
fi

local_paths="$(grep -REn --include='*.cpp' --include='*.h' --include='*.sh' --include='*.ps1' 'S:\\+VisualStudio|C:\\+Users|/Users/[A-Za-z]|AppData[\\/]Local[\\/]Temp' src tools/*.sh tools/*.ps1 2>/dev/null | grep -v 'tools/test' || true)"
if [ -z "$local_paths" ]; then
  record Hygiene "no local absolute paths in source or scripts" PASS ""
else
  record Hygiene "no local absolute paths in source or scripts" FAIL "$(printf '%s\n' "$local_paths" | head -n 2 | tr '\n' ' ')"
fi

# ---- Group summary ----------------------------------------------------------------------------------------------------
echo
echo "== Failures"
grep $'\tFAIL\t' "$RESULTS" | while IFS=$'\t' read -r group name status detail; do
  echo "FAIL $group / $name${detail:+: $detail}"
done
echo
echo "== Not run"
grep $'\tNOT RUN\t' "$RESULTS" | while IFS=$'\t' read -r group name status detail; do
  echo "NOT RUN $group / $name${detail:+: $detail}"
done
echo
echo "== Groups"
declare -A group_total=() group_fail=() group_skip=()
while IFS=$'\t' read -r group name status detail; do
  group_total["$group"]=$(( ${group_total["$group"]:-0} + 1 ))
  [ "$status" = "FAIL" ] && group_fail["$group"]=$(( ${group_fail["$group"]:-0} + 1 ))
  [ "$status" = "NOT RUN" ] && group_skip["$group"]=$(( ${group_skip["$group"]:-0} + 1 ))
done < "$RESULTS"

failed_groups=()
for group in "${groups[@]}"; do
  total="${group_total[$group]:-0}"
  fail="${group_fail[$group]:-0}"
  skip="${group_skip[$group]:-0}"
  if [ "$total" -eq 0 ]; then
    echo "FAIL     $group (no check ran)"
    failed_groups+=("$group")
  elif [ "$fail" -gt 0 ]; then
    echo "FAIL     $group ($fail failed, $skip not run, $((total - fail - skip)) passed)"
    failed_groups+=("$group")
  elif [ "$skip" -gt 0 ]; then
    echo "NOT RUN  $group ($skip not run, $((total - skip)) passed)"
  else
    echo "PASS     $group ($total checks)"
  fi
done

total="$(wc -l < "$RESULTS" | tr -d ' ')"
fail_count="$(grep -c $'\tFAIL\t' "$RESULTS" || true)"
skip_count="$(grep -c $'\tNOT RUN\t' "$RESULTS" || true)"
pass_count=$((total - fail_count - skip_count))
echo
echo "$total checks: $pass_count passed, $fail_count failed, $skip_count not run"
if [ "${#failed_groups[@]}" -gt 0 ]; then
  echo "Failed groups: ${failed_groups[*]}"
fi
if [ "$RELEASE" -eq 1 ]; then
  echo "Release gate: only the DEBUG_SAULO diagnostics marked release in tools/debug-markers.txt are accepted."
fi
[ "$fail_count" -eq 0 ]
