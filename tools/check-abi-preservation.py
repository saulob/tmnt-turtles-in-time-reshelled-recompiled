#!/usr/bin/env python3
"""Conservative PPC nonvolatile-register audit for generated functions."""

import re
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "build" / "abi-preservation-report.txt"

FUNC_RE = re.compile(r"^DEFINE_REX_FUNC\(([^)]+)\)\s*\{", re.M)
NV_RE = re.compile(r"ctx\.r(1[4-9]|2[0-9]|3[01])\.[A-Za-z0-9_]+\s*=")
SAVE_RE = re.compile(
    r"__savegpr|__savegprlr|REX_STORE_U(?:32|64)\(ctx\.r1\.u32 \+ -\d+, ctx\.r(?:1[4-9]|2[0-9]|3[01])\.u")
CONTEXT_RE = re.compile(
    r"RtlUnwind|ppc_longjmp|ctx\.r1\.u64 = REX_LOAD_U64\(ctx\.r\d+\.u32 \+ \d+\)")


def first(body, needles):
    at = -1
    for needle in needles:
        pos = body.find(needle)
        if pos >= 0 and (at < 0 or pos < at):
            at = pos
    return at


def reads_before_write(body, reg):
    write = first(body, [f"ctx.r{reg}.{s} =" for s in ("u64", "s64", "u32", "s32", "f64", "f32")])
    read = first(body, [f"ctx.r{reg}.u32 +", f"REX_LOAD_U32(ctx.r{reg}.u32",
                        f"REX_LOAD_U64(ctx.r{reg}.u32", f"ctx.r{reg}.u64 +",
                        f"ctx.r{reg}.s64 +"])
    return read >= 0 and (write < 0 or read < write)


def scan(path):
    data = path.read_text(encoding="utf-8", errors="replace")
    high, review = [], []
    scanned = 0
    for m in FUNC_RE.finditer(data):
        name = m.group(1)
        start = m.end()
        end = data.find("\n}\n", start)
        if end < 0:
            continue
        body = data[start:end]
        if name.startswith("__"):
            continue
        writes = sorted({int(x) for x in NV_RE.findall(body)})
        if not writes:
            continue
        scanned += 1
        if SAVE_RE.search(body) or CONTEXT_RE.search(body):
            continue
        if re.search(r"ctx\.r(?:29|30|31)\.(?:s64|u64) = ctx\.r12\.(?:s64|u64)", body):
            review.append((name, "funclet-frame"))
            continue
        if reads_before_write(body, 31):
            review.append((name, "live-in-r31"))
            continue
        if any(reads_before_write(body, reg) for reg in writes):
            review.append((name, "live-in-reg"))
            continue
        if any(re.search(rf"ctx\.r{reg}\.\w+ = .*ctx\.r{reg}\.", body) for reg in writes):
            review.append((name, "self-update"))
            continue
        high.append((name, writes))
    return high, review, scanned


def main():
    high_all, review_all = [], []
    scanned = 0
    for mod in ("default", "tmnt_r"):
        directory = ROOT / "generated" / mod
        if not directory.exists():
            continue
        for path in sorted(directory.glob("*.cpp")):
            high, review, count = scan(path)
            scanned += count
            high_all.extend((mod, path.name, name, writes) for name, writes in high)
            review_all.extend((mod, path.name, name, why) for name, why in review)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("w", encoding="utf-8") as out:
        out.write("# module file function nonvolatile_writes_without_save\n")
        for mod, name_file, name, writes in high_all:
            out.write(f"HIGH\t{mod}\t{name_file}\t{name}\t{','.join(f'r{w}' for w in writes)}\n")
        for mod, name_file, name, why in review_all:
            out.write(f"REVIEW\t{mod}\t{name_file}\t{name}\t{why}\n")
    print(f"scanned={scanned} high={len(high_all)} review={len(review_all)} report={OUT}")
    for row in high_all[:20]:
        print("HIGH", row)


if __name__ == "__main__":
    main()
