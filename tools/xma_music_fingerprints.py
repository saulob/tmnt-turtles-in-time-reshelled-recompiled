"""Fingerprints every XMA stream in the local TMNT.bf and checks that the Classic Soundtrack music fingerprints are unique.

Read-only. Prints keys, sizes and fingerprints only, never audio data.
usage: python tools/xma_music_fingerprints.py [game/extracted/Data/TMNT.bf]
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bf_extract  # noqa: E402

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
HASHED_BYTES = 4096

# Must match kMusicStreams in src/classic_soundtrack/music_streams.h.
MUSIC_KEYS = {
    0x04401670: "StartMenu",
    0x04401665: "01 Big Apple 3 AM",
    0x04401666: "02 Alley Cat Blues",
    0x04401668: "03 Sewer Surfin'",
    0x04401669: "04 Prehistoric Turtlesaurus",
    0x0440166A: "05 Skull and Crossbones",
    0x0440166B: "06 Bury My Shell at Wounded Knee",
    0x0440166C: "07 Neon Night-Riders",
    0x0440166D: "08 Starbase",
    0x0440166E: "09 Technodrome",
    0x0440166F: "boss",
    0x04401675: "stage clear",
    0x04401672: "time warp",
    0x0440168C: "outro",
}


def fingerprint(payload):
    value = FNV_OFFSET
    for byte in struct.pack("<I", len(payload)) + payload[:HASHED_BYTES]:
        value = ((value ^ byte) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value


def bundles(archive):
    for record in bf_extract.read_records(archive):
        name = record[1]
        if not (name.startswith("fdd") and name.endswith(".bin")):
            continue
        data = bf_extract.entry_bytes(archive, record)
        if data[:4] == b"CPBK":
            compressed = struct.unpack_from("<I", data, 4)[0]
            data = bf_extract.lzo1x_decompress(data[12:12 + compressed])
        yield name, data


def streams(name, data):
    count = struct.unpack_from("<I", data, 0)[0]
    at = 4
    for _ in range(count):
        key, size = struct.unpack_from("<II", data, at)
        yield key, data[at + 8:at + 8 + size]
        at += 8 + size


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join("game", "extracted", "Data", "TMNT.bf")
    archive = open(path, "rb").read()
    seen = {}
    total = 0
    for name, data in bundles(archive):
        for key, payload in streams(name, data):
            total += 1
            seen.setdefault(fingerprint(payload), set()).add((key, len(payload)))
    collisions = [fp for fp, owners in seen.items() if len({key for key, _ in owners}) > 1]
    by_key = {}
    for fp, owners in seen.items():
        for key, size in owners:
            by_key.setdefault(key, set()).add((fp, size))
    print("streams %d, distinct fingerprints %d, collisions between different keys %d" % (total, len(seen), len(collisions)))
    failures = 0
    for fp in collisions:
        keys = sorted({key for key, _ in seen[fp]})
        music = [key for key in keys if key in MUSIC_KEYS]
        print("  0x%016X shared by %s%s" % (fp, ", ".join("0x%08X" % key for key in keys),
                                          "  MUSIC COLLISION" if music else "  (no music stream, identical data)"))
        failures += 1 if music else 0
    for key, label in MUSIC_KEYS.items():
        found = by_key.get(key, set())
        if len(found) != 1:
            print("MISSING or ambiguous 0x%08X %s (%d variants)" % (key, label, len(found)))
            failures += 1
            continue
        fp, size = next(iter(found))
        print("0x%08X  size %8d  fingerprint 0x%016X  %s" % (key, size, fp, label))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
