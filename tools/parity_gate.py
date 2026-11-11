#!/usr/bin/env python3
"""Shared static gate for the Windows/Linux feature matrix.

Both tools/test.ps1 and tools/test.sh run this script, so the two gates cannot
disagree about which portable features must exist on Linux. It reads
tools/test-groups.txt and tools/parity-manifest.txt and writes one tab-separated
line per check:

    CHECK<TAB>group<TAB>name<TAB>PASS|FAIL|NOT RUN<TAB>detail

Exit code 1 when any check fails. A pending feature is NOT RUN, never PASS.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

CHECK = "CHECK"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace").replace("\r", "")


def condition(text: str) -> tuple[bool, bool]:
    """(body is Windows-only, body is non-Windows-only) for one #if directive."""
    stripped = text.lstrip()
    word = stripped.split(None, 1)[0] if stripped else ""
    compact = text.replace(" ", "").replace("\t", "")
    if word in ("ifdef", "ifndef"):
        name = stripped.split(None, 1)[1] if len(stripped.split(None, 1)) > 1 else ""
        if name in ("_WIN32", "WIN32"):
            return word == "ifdef", word == "ifndef"
        if name in ("__linux__", "__unix__", "__APPLE__"):
            return False, word == "ifdef"
        return False, False
    negated = "!defined(_WIN32)" in compact or "!defined(WIN32)" in compact
    linux = "__linux__" in compact or "__unix__" in compact or "__APPLE__" in compact
    windows = "_WIN32" in compact or "defined(WIN32)" in compact
    if windows:
        return not negated, negated
    return False, linux


def cmake_platform_sources(text: str) -> tuple[str, str, str]:
    """(Windows-only, non-Windows-only, common) lines for a CMake file."""
    windows: list[str] = []
    linux: list[str] = []
    common: list[str] = []
    stack: list[dict[str, object]] = []
    for line in text.split("\n"):
        low = line.strip().lower()
        if low.startswith(("if(", "if (")) and ")" in low:
            inner = low[low.find("(") + 1 : low.rfind(")")].replace(" ", "")
            cond = "other"
            if "win32" in inner:
                cond = "linux" if ("notwin32" in inner or "!win32" in inner) else "windows"
            stack.append({"cond": cond, "else": False})
            continue
        if low.startswith(("else(", "elseif(")):
            if stack:
                stack[-1]["else"] = True
                if low.startswith("elseif(") and ")" in low:
                    inner = low[low.find("(") + 1 : low.rfind(")")].replace(" ", "")
                    if "win32" in inner:
                        stack[-1]["cond"] = "linux" if ("notwin32" in inner or "!win32" in inner) else "windows"
            continue
        if low.startswith("endif("):
            if stack:
                stack.pop()
            continue
        active = "common"
        for frame in stack:
            cond = frame["cond"]
            if frame["else"]:
                if cond == "windows":
                    cond = "linux"
                elif cond == "linux":
                    cond = "windows"
            if cond in ("windows", "linux"):
                active = cond
        (windows if active == "windows" else linux if active == "linux" else common).append(line)
    return "\n".join(windows), "\n".join(linux), "\n".join(common)


def non_windows_text(text: str) -> str:
    """The text a non-Windows build sees: Windows-only #if branches are removed."""
    kept: list[str] = []
    stack: list[dict[str, bool]] = []
    for line in text.split("\n"):
        stripped = line.lstrip()
        if stripped.startswith("#"):
            directive = stripped[1:].strip()
            word = directive.split(None, 1)[0] if directive else ""
            if word in ("if", "ifdef", "ifndef"):
                windows, else_windows = condition(directive)
                stack.append({"windows": windows, "else_windows": else_windows, "else": False})
                continue
            if word in ("else", "elif") and stack:
                stack[-1]["else"] = True
                continue
            if word == "endif" and stack:
                stack.pop()
                continue
        excluded = any(
            (frame["windows"] and not frame["else"]) or (frame["else_windows"] and frame["else"])
            for frame in stack
        )
        if not excluded:
            kept.append(line)
    return "\n".join(kept)


def has_all(text: str, needles: list[str]) -> list[str]:
    return [needle for needle in needles if needle not in text]


def parse_groups(root: Path) -> list[str]:
    groups: list[str] = []
    for line in read(root / "tools/test-groups.txt").split("\n"):
        line = line.strip()
        if line and not line.startswith("#"):
            groups.append(line)
    return groups


def parse_manifest(root: Path) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for number, line in enumerate(read(root / "tools/parity-manifest.txt").split("\n"), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) not in (5, 6):
            rows.append({"kind": "invalid", "line": number, "raw": line})
            continue
        if len(fields) == 5:
            fields.append("")
        kind, group, name, path, needles, extra = fields
        rows.append(
            {
                "kind": kind,
                "group": group,
                "name": name,
                "path": path,
                "needles": [needle for needle in needles.split(";") if needle],
                "extra": extra,
                "line": number,
            }
        )
    return rows


def emit(group: str, name: str, status: str, detail: str = "") -> None:
    print(f"{CHECK}\t{group}\t{name}\t{status}\t{detail}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    parser.add_argument("--groups", action="store_true", help="print the group list and exit")
    args = parser.parse_args()
    root = Path(args.root).resolve()

    groups = parse_groups(root)
    if args.groups:
        print("\n".join(groups))
        return 0
    rows = parse_manifest(root)
    cmake = read(root / "CMakeLists.txt")
    failures = 0
    seen: dict[str, int] = {group: 0 for group in groups}

    def report(group: str, name: str, ok: bool, detail: str) -> None:
        nonlocal failures
        if ok:
            emit(group, name, "PASS")
        else:
            failures += 1
            emit(group, name, "FAIL", detail)

    # The manifest and the group list must describe the same gate.
    for row in rows:
        if row["kind"] == "invalid":
            report("Core", f"manifest line {row['line']}", False, "not six tab-separated columns")
            continue
        group = str(row["group"])
        if group not in seen:
            report("Core", f"manifest line {row['line']}", False, f"unknown group '{group}'")
        else:
            seen[group] += 1
    for group in groups:
        report("Core", f"group {group} has checks", seen[group] > 0, "no row in tools/parity-manifest.txt")

    source_files = sorted(
        path for path in (root / "src").rglob("*") if path.suffix in (".cpp", ".c") and path.is_file()
    )
    orphans = [
        path.relative_to(root).as_posix()
        for path in source_files
        if path.relative_to(root).as_posix() not in cmake
    ]
    report(
        "Cross-platform parity",
        "every source is built or declared dormant",
        not orphans,
        "not in CMakeLists.txt: " + ", ".join(orphans[:8]),
    )

    patch_dir = root / "patches/rexglue-sdk"
    patch_rows = [
        line.split()
        for line in read(patch_dir / "patches.txt").split("\n")
        if line.strip() and not line.lstrip().startswith("#")
    ]
    listed_patches = [fields[0] for fields in patch_rows if fields and fields[0].endswith(".patch")]
    patch_files = sorted(path.name for path in patch_dir.glob("*.patch"))
    missing_patch_files = [name for name in listed_patches if name not in patch_files]
    unlisted_patch_files = [name for name in patch_files if name not in listed_patches]
    malformed_patch_rows = [fields[0] for fields in patch_rows if len(fields) != 5]
    missing_patch_files += malformed_patch_rows

    report(
        "Core",
        "patch manifest matches the patch files",
        not (missing_patch_files or unlisted_patch_files),
        "missing: " + ", ".join(missing_patch_files[:6]) + " unlisted: " + ", ".join(unlisted_patch_files[:6]),
    )

    platform_only: list[str] = []
    for path in sorted((root / "src").rglob("*")):
        if not path.is_file() or path.suffix not in (".cpp", ".h", ".c"):
            continue
        relative = path.relative_to(root).as_posix()
        stack: list[bool] = []
        for number, line in enumerate(read(path).split("\n"), 1):
            stripped = line.lstrip()
            if stripped.startswith("#"):
                directive = stripped[1:].strip()
                word = directive.split(None, 1)[0] if directive else ""
                if word in ("if", "ifdef", "ifndef"):
                    windows, else_windows = condition(directive)
                    stack.append({"windows": windows, "else_windows": else_windows, "else": False})
                    continue
                if word in ("else", "elif") and stack:
                    stack[-1]["else"] = True
                    continue
                if word == "endif" and stack:
                    stack.pop()
                    continue
            if not any(
                (frame["windows"] and not frame["else"]) or (frame["else_windows"] and frame["else"])
                for frame in stack
            ):
                continue
            if "REXCVAR_DEFINE" in stripped or "REX_HOOK_RAW(" in stripped:
                platform_only.append(f"{relative}:{number}")
    report(
        "Cross-platform parity",
        "no setting or hook is inside a Windows-only branch",
        not platform_only,
        ", ".join(platform_only[:8]),
    )

    # A platform-only CMake source list may only hold a platform implementation with a named counterpart.
    pairs = {
        "src/online/websocket_win.cpp": "src/online/websocket_posix.cpp",
        "src/clipboard_win32.cpp": "src/clipboard_sdl.cpp",
    }
    # The Windows ZIP helper executable: Linux replaces the AppImage directly in update_platform.cpp.
    documented_windows_only = set(pairs) | {"src/updater/helper_main.cpp"}
    documented_linux_only = set(pairs.values())
    windows_cmake, linux_cmake, common_cmake = cmake_platform_sources(cmake)
    source_pattern = r"src/[A-Za-z0-9_./-]+\.(?:cpp|c)\b"
    windows_sources = set(re.findall(source_pattern, windows_cmake))
    linux_sources = set(re.findall(source_pattern, linux_cmake))
    common_sources = set(re.findall(source_pattern, common_cmake))
    unpaired_windows = sorted(path for path in windows_sources - common_sources if path not in documented_windows_only)
    unpaired_linux = sorted(path for path in linux_sources - common_sources if path not in documented_linux_only)
    report(
        "Cross-platform parity",
        "every Windows-only CMake source has a Linux counterpart",
        not unpaired_windows,
        ", ".join(unpaired_windows[:8]),
    )
    report(
        "Cross-platform parity",
        "every Linux-only CMake source has a Windows counterpart",
        not unpaired_linux,
        ", ".join(unpaired_linux[:8]),
    )
    missing_pairs = [counterpart for counterpart in pairs.values() if counterpart not in cmake]
    report(
        "Cross-platform parity",
        "every platform source pair is built",
        not missing_pairs,
        ", ".join(missing_pairs[:8]),
    )

    # Both gates must read the same matrix; a private list would let Linux fall behind Windows.
    test_ps1 = read(root / "tools/test.ps1")
    test_sh = read(root / "tools/test.sh")
    gate_users = [name for name, text in (("tools/test.ps1", test_ps1), ("tools/test.sh", test_sh))
                  if "parity_gate.py" in text and "test-groups.txt" in text]
    report(
        "Cross-platform parity",
        "both test gates read the shared feature matrix",
        len(gate_users) == 2,
        "do not run tools/parity_gate.py: " + ", ".join(name for name in ("tools/test.ps1", "tools/test.sh")
                                                        if name not in gate_users),
    )

    for row in rows:
        if row["kind"] == "invalid":
            continue
        kind = str(row["kind"])
        group = str(row["group"])
        name = str(row["name"])
        path = str(row["path"])
        needles = [str(needle) for needle in row["needles"]]
        extra = str(row["extra"])
        target = root / path
        if kind == "pending":
            if extra and extra in cmake:
                report(group, name, False, f"{extra} is now in the build: turn this pending row into a feature row")
            else:
                emit(group, name, "NOT RUN", needles[0] if needles else "not implemented on this branch")
            continue
        if kind not in ("feature", "pair", "test", "contract", "present"):
            report(group, name, False, f"unknown manifest kind '{kind}'")
            continue
        if not target.exists():
            report(group, name, False, f"{path} does not exist")
            continue
        if kind == "present":
            missing = has_all(read(target), needles)
            report(group, name, not missing, f"{path} lacks: " + ", ".join(missing[:6]))
            continue
        if kind == "pair":
            counterpart = root / extra
            if not counterpart.exists():
                report(group, name, False, f"{extra} does not exist")
            elif path not in cmake or extra not in cmake:
                report(group, name, False, "one platform's source is not in CMakeLists.txt")
            else:
                report(group, name, True, "")
            continue
        if kind == "test":
            if f"add_executable({extra}" not in cmake:
                report(group, name, False, f"CMake target {extra} is not registered")
            else:
                report(group, name, True, "")
            continue
        if kind == "feature" and target.suffix in (".cpp", ".c") and path not in cmake:
            report(group, name, False, f"{path} is not compiled on both platforms")
            continue
        if target.is_dir():
            entries = {entry.name for entry in target.rglob("*") if entry.is_file()}
            missing = [needle for needle in needles if not any(needle in entry for entry in entries)]
            text = ""
        else:
            text = non_windows_text(read(target))
            missing = has_all(text, needles)
        report(group, name, not missing, f"{path} lacks outside Windows-only code: " + ", ".join(missing[:6]))

    report(
        "Cross-platform parity",
        "both updater package kinds exist",
        "UpdatePackageKind::kZip" in read(root / "src/update_config.cpp")
        and "UpdatePackageKind::kAppImage" in read(root / "src/update_config.cpp"),
        "update_config.cpp lost the Windows ZIP or the Linux AppImage",
    )

    # Launcher pages are shared, so the sidebar cannot lose one on Linux.
    launcher = non_windows_text(read(root / "src/launcher.cpp"))
    pages = [
        "kPlay",
        "kDisplay",
        "kGraphics",
        "kGameplay",
        "kSnes",
        "kCheats",
        "kOnline",
        "kAchievements",
        "kControls",
        "kBugReport",
        "kAbout",
    ]
    missing_pages = [page for page in pages if f"MakePage(LauncherPage::{page}" not in launcher]
    report(
        "Launcher",
        "every launcher page is built on both platforms",
        not missing_pages,
        "missing: " + ", ".join(missing_pages),
    )
    positions = [launcher.find(f"MakePage(LauncherPage::{page}") for page in pages]
    ordered = all(position >= 0 for position in positions) and positions == sorted(positions)
    report(
        "Launcher",
        "launcher pages keep their order on both platforms",
        ordered,
        "the MakePage calls are not in Play..About order",
    )
    report(
        "Launcher",
        "the DLC page still inserts after Play",
        "c.pages.insert(c.pages.begin() + 1, MakePage(LauncherPage::kDlc" in launcher,
        "the DLC page moved or disappeared",
    )
    launcher_doc = read(root / "docs/launcher.md")
    report(
        "Launcher",
        "the Graphics card row is documented for both platforms",
        "d3d12_adapter" in launcher_doc
        and "vulkan_device" in launcher_doc
        and "shown on Windows only" not in launcher_doc,
        "docs/launcher.md still describes the Graphics card as Windows-only",
    )

    # Protocol constants are shared: one source, no platform branch.
    protocol = read(root / "src/online/protocol.cpp")
    report(
        "Online",
        "online protocol is shared",
        "src/online/protocol.cpp" in cmake and "_WIN32" not in protocol and "__linux__" not in protocol,
        "protocol.cpp is platform-specific or unbuilt",
    )

    # The Linux contracts that are easiest to lose without a compiler.
    contracts = [
        ("user_data follows the AppImage", "src/platform.cpp", 'getenv("APPIMAGE")'),
        ("logs live in user_data", "src/log_path.cpp", 'user_data_root / "logs"'),
        ("relaunch restarts the AppImage", "src/platform.cpp", "posix_spawn"),
        ("crash handlers are installed", "src/crash_report.cpp", "InstallPosixHandlers"),
        ("the updater replaces the AppImage", "src/updater/update_platform.cpp", "ApplyAppImageUpdate"),
        ("Bug Report says Linux", "src/incident_report.cpp", '\\"platform\\":\\"Linux\\"'),
        ("the launcher has every page", "src/launcher.cpp", "LauncherPage::kBugReport"),
    ]
    for name, path, needle in contracts:
        text = non_windows_text(read(root / path))
        report("Linux contracts", name, needle in text, f"{path} lacks {needle}")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
