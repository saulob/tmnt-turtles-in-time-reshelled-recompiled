#!/usr/bin/env python3
"""Read-only extractor for TMNT.bf entries (header + 200-byte records, LZO1X payloads)."""
import argparse
import re
import struct
import sys

HEADER_SIZE = 5184
FOLDER_SIZE = 12
RECORD_SIZE = 200
PAYLOAD_HEADER = 32


def lzo1x_decompress(src):
    out = bytearray()
    ip = 0

    def extended(base):
        nonlocal ip
        n = base
        while src[ip] == 0:
            n += 255
            ip += 1
        n += src[ip]
        ip += 1
        return n

    state = 0
    first = src[0]
    if first > 17:
        n = first - 17
        out += src[1:1 + n]
        ip = 1 + n
        state = n if n < 4 else 4
    while True:
        t = src[ip]
        ip += 1
        if t >= 64:
            dist = ((t >> 2) & 7) + (src[ip] << 3) + 1
            ip += 1
            length = (t >> 5) + 1
            trail = t & 3
        elif t >= 32:
            length = (t & 31) or extended(31)
            length += 2
            d = src[ip] | (src[ip + 1] << 8)
            ip += 2
            dist = (d >> 2) + 1
            trail = d & 3
        elif t >= 16:
            length = (t & 7) or extended(7)
            length += 2
            d = src[ip] | (src[ip + 1] << 8)
            ip += 2
            dist = ((t & 8) << 11) + (d >> 2)
            if dist == 0:
                return bytes(out)
            dist += 0x4000
            trail = d & 3
        elif state == 0:
            n = (t + 3) if t else extended(15) + 3
            out += src[ip:ip + n]
            ip += n
            state = 4
            continue
        else:
            dist = (t >> 2) + (src[ip] << 2) + (1 if state < 4 else 2049)
            ip += 1
            length = 2 if state < 4 else 3
            trail = t & 3
        start = len(out) - dist
        if start < 0:
            raise ValueError("match before start of output")
        for i in range(length):
            out.append(out[start + i])
        out += src[ip:ip + trail]
        ip += trail
        state = trail


def read_records(data):
    count, _, _ = struct.unpack_from("<3I", data, HEADER_SIZE)
    base = HEADER_SIZE + FOLDER_SIZE
    for i in range(count):
        rec = data[base + RECORD_SIZE * i: base + RECORD_SIZE * (i + 1)]
        name = rec[:48].split(b"\0")[0].decode("latin-1")
        size, = struct.unpack_from("<I", rec, 88)
        crc, = struct.unpack_from("<I", rec, 100)
        _, offset = struct.unpack_from("<II", rec, 104)
        flags, = struct.unpack_from("<H", rec, 112)
        yield i, name, size, crc, offset, flags


def entry_bytes(data, record):
    _, _, size, _, offset, _ = record
    payload = data[offset:offset + size]
    csize, usize = struct.unpack_from("<2I", payload, 0)
    body = payload[PAYLOAD_HEADER:PAYLOAD_HEADER + csize]
    return lzo1x_decompress(body) if csize < usize else body


def _find_all(text, pattern):
    i = text.find(pattern)
    while i >= 0:
        yield i
        i = text.find(pattern, i + 1)


def native_call_sites(data, native):
    """(entry, function, ordinal, offset) of each call record naming `native` in compiled libraries."""
    records = {r[1]: r for r in read_records(data)}
    name = native.encode()
    words = (len(name) + 4) // 4
    call_record = b"\x22\x08\xff\xff\xff\xff" + struct.pack("<I", words) + name + b"\0"
    sites = []
    for entry, record in records.items():
        if not entry.endswith((".efl", ".efc")):
            continue
        compiled = entry_bytes(data, record)
        if call_record not in compiled:
            continue
        base = entry.rsplit(".", 1)[0]
        source_record = records.get(base + ".fcl") or records.get(base + ".fct")
        source = entry_bytes(data, source_record) if source_record else b""
        names = {m.group(1) for m in re.finditer(rb"function\s+(?:public\s+|private\s+)?\w+\s+(\w+)\s*\(", source)}
        if entry.endswith(".efc"):
            names = {base.encode()}
        heads = sorted(
            (i, fn.decode()) for fn in names for i in _find_all(compiled, struct.pack("<I", len(fn)) + fn)
            if not chr(compiled[i + 4 + len(fn)]).isalnum())
        ordinals = {}
        for offset in _find_all(compiled, call_record):
            owner = [h for h in heads if h[0] < offset]
            function = owner[-1][1] if owner else "?"
            ordinals[function] = ordinals.get(function, 0) + 1
            sites.append((entry, function, ordinals[function], offset))
    return sites


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bf")
    ap.add_argument("name", nargs="?")
    ap.add_argument("-o", "--out")
    ap.add_argument("-l", "--list", action="store_true")
    ap.add_argument("--calls", metavar="NATIVE", help="list compiled call sites of a native")
    a = ap.parse_args()
    with open(a.bf, "rb") as f:
        data = f.read()
    if data[:4] != b"ABE\0":
        sys.exit("not a BF archive")
    if a.calls:
        for site in native_call_sites(data, a.calls):
            print(*site)
        return
    recs = list(read_records(data))
    if a.list or not a.name:
        for r in recs:
            print(*r)
        return
    match = [r for r in recs if r[1] == a.name]
    if not match:
        sys.exit("entry not found")
    _, name, size, crc, offset, flags = match[0]
    payload = data[offset:offset + size]
    csize, usize, extra, hflags = struct.unpack_from("<4I", payload, 0)
    body = payload[PAYLOAD_HEADER:PAYLOAD_HEADER + csize]
    if csize < usize:
        text = lzo1x_decompress(body)
        if len(text) != usize:
            sys.exit("size mismatch %d != %d" % (len(text), usize))
    else:
        text = body
    sys.stderr.write("%s stored=%d csize=%d usize=%d extra=%d flags=%d rflags=%d\n" % (name, size, csize, usize, extra, hflags, flags))
    if a.out:
        with open(a.out, "wb") as f:
            f.write(text)
    else:
        sys.stdout.buffer.write(text)


if __name__ == "__main__":
    main()
