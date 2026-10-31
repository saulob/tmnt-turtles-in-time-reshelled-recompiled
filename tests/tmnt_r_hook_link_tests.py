"""Checks that src/live_hooks_tmnt_r.cpp is linked into the TMNT_r DLL and overrides the generated functions.

Needs a configured build tree (build/) for the link checks and game/analysis plus game/extracted for the address
checks; each group is skipped when its inputs are missing.
"""
import glob
import os
import re
import struct
import subprocess
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD = os.environ.get("TMNT_BUILD_DIR", os.path.join(ROOT, "build"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import bf_extract as bf  # noqa: E402

TARGET = "tmnt_turtles_in_time_reshelled_TMNT_r"
HOOK_OBJ = os.path.join(BUILD, "CMakeFiles", TARGET + ".dir", "src", "live_hooks_tmnt_r.cpp.obj")
DLL = os.path.join(BUILD, TARGET + ".dll")
NINJA = os.path.join(BUILD, "build.ninja")
IMAGE = os.path.join(ROOT, "game", "analysis", "tmnt_r.image.bin")
BF = os.path.join(ROOT, "game", "extracted", "Data", "TMNT.bf")
GENERATED = os.path.join(ROOT, "generated", "tmnt_r")
MODULE_BASE = 0x87500000

HOOKED = {
    0x876AE008: "PLT_IsSignedIntoFullAccountOnline",
    0x8759F858: "Leaderboard_SwitchPage",
    0x876AE720: "PLT_CheckPlayerSignIn",
    0x876B75C0: "StartMenuWorld_CheckSignIn",
    0x875BA998: "FireCommunicator_OnReceiveEvent",
    0x875A6770: "SessionWorld_RequestWriteStatsAllValid",
    0x87599B80: "InGameHUD_GoShowSummary",
    0x875BA650: "FireCommunicator_SendCharSelGamerTag",
    0x8757E9E0: "BasicWorld_CheckIfPlayerCanViewProfile",
    0x87520018: "engine import lookup",
}
UNHOOKED_CONTROL = 0x876AE628


def read_file(path, mode="rb", **kwargs):
    with open(path, mode, **kwargs) as f:
        return f.read()


def llvm_tool(name):
    cache = os.path.join(BUILD, "CMakeCache.txt")
    with open(cache, encoding="utf-8") as f:
        match = re.search(r"^CMAKE_NM:FILEPATH=(.+)$", f.read(), re.M)
    return os.path.join(os.path.dirname(match.group(1)), name + ".exe")


def nm(path):
    out = subprocess.run([llvm_tool("llvm-nm"), path], capture_output=True, text=True, check=True).stdout
    symbols = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-1].startswith(("sub_", "__imp__sub_")):
            symbols.setdefault(parts[-1], []).append(parts[-2])
    return symbols


def sub(address):
    return "sub_%08X" % address


@unittest.skipUnless(os.path.exists(NINJA) and os.path.exists(HOOK_OBJ), "configured build tree not present")
class LinkGraphTests(unittest.TestCase):
    def link_line(self, output):
        with open(NINJA, encoding="utf-8") as f:
            for line in f:
                if line.startswith("build " + output + " ") or line.startswith("build " + output + ":"):
                    return line
        self.fail("no link statement for " + output)

    def test_hook_object_is_in_the_tmnt_r_dll_link(self):
        line = self.link_line(TARGET + ".dll")
        self.assertIn("generated/tmnt_r/tmnt_turtles_in_time_reshelled_recomp.19.cpp.obj", line)
        self.assertIn(TARGET + ".dir/src/live_hooks_tmnt_r.cpp.obj", line)

    def test_hook_object_is_not_in_the_host_link(self):
        line = self.link_line("tmnt_turtles_in_time_reshelled.exe")
        self.assertNotIn("live_hooks_tmnt_r", line)

    def test_dll_is_not_older_than_the_hook_object(self):
        self.assertGreaterEqual(os.path.getmtime(DLL), os.path.getmtime(HOOK_OBJ))

    def test_hook_symbols_are_strong_and_unique(self):
        symbols = nm(HOOK_OBJ)
        for address in HOOKED:
            self.assertEqual(symbols.get(sub(address)), ["T"], sub(address))

    def test_generated_definitions_are_weak_aliases_with_an_original_entry(self):
        generated = {}
        for path in glob.glob(os.path.join(BUILD, "CMakeFiles", TARGET + ".dir", "generated", "tmnt_r", "*recomp.*.obj")):
            for name, kinds in nm(path).items():
                generated.setdefault(name, []).extend(kind for kind in kinds if kind != "U")
        for address in list(HOOKED) + [UNHOOKED_CONTROL]:
            self.assertEqual(sorted(generated.get(sub(address), [])), ["W"], sub(address))
            self.assertEqual(generated.get("__imp__" + sub(address)), ["T"], sub(address))

    def test_dll_function_table_points_at_the_hooks(self):
        data = read_file(DLL)
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        optional = pe + 24
        image_base = struct.unpack_from("<Q", data, optional + 24)[0]
        symbolizer = llvm_tool("llvm-symbolizer")
        for address, name in list(HOOKED.items()) + [(UNHOOKED_CONTROL, "control")]:
            pattern = struct.pack("<Q", address)
            host = None
            at = data.find(pattern)
            while at >= 0 and host is None:
                candidate = struct.unpack_from("<Q", data, at + 8)[0]
                if image_base <= candidate < image_base + 0x2000000:
                    host = candidate
                at = data.find(pattern, at + 1)
            self.assertIsNotNone(host, name)
            out = subprocess.run([symbolizer, "--obj=" + DLL, "--no-inlines", "--functions=short", "--basenames", hex(host)],
                                 capture_output=True, text=True, check=True).stdout
            expected = ("__imp__" + sub(address)) if address == UNHOOKED_CONTROL else sub(address)
            self.assertEqual(out.splitlines()[0], expected, name)
            if address != UNHOOKED_CONTROL:
                self.assertIn("live_hooks_tmnt_r.cpp", out, name)


def function_facts(name):
    """C strings and engine import indices the generated function refers to, and the DLL functions it calls."""
    image = read_file(IMAGE)
    body = None
    for path in glob.glob(os.path.join(GENERATED, "*recomp.*.cpp")):
        text = read_file(path, "r", encoding="utf-8", errors="replace")
        start = text.find("DEFINE_REX_FUNC(%s) {" % name)
        if start >= 0:
            end = text.find("\nDEFINE_REX_FUNC(", start + 1)
            body = text[start:end if end > 0 else len(text)]
            break
    assert body is not None, name
    registers, strings, imports, calls = {}, [], [], []
    last_r3 = None
    for line in body.splitlines():
        m = re.search(r"ctx\.r(\d+)\.s64 = (-?\d+);", line)
        if m:
            registers[int(m.group(1))] = int(m.group(2))
            if int(m.group(1)) == 3:
                last_r3 = int(m.group(2))
        m = re.search(r"ctx\.r(\d+)\.s64 = ctx\.r(\d+)\.s64 \+ (-?\d+);", line)
        if m and int(m.group(2)) in registers:
            offset = ((registers[int(m.group(2))] + int(m.group(3))) & 0xFFFFFFFF) - MODULE_BASE
            if 0 <= offset < len(image):
                end = image.find(b"\0", offset, offset + 200)
                text = image[offset:end]
                if len(text) >= 4 and all(32 <= c < 127 for c in text):
                    strings.append(text.decode())
        m = re.search(r"\b(sub_[0-9A-F]{8})\(ctx, base\)", line)
        if m:
            if m.group(1) == "sub_87520018":
                imports.append(last_r3)
            else:
                calls.append(m.group(1))
    return strings, imports, calls


@unittest.skipUnless(os.path.exists(IMAGE) and os.path.isdir(GENERATED), "TMNT_r analysis files not present")
class AddressTests(unittest.TestCase):
    def test_entries_start_like_functions(self):
        image = read_file(IMAGE)
        for address in HOOKED:
            if address == 0x87520018:
                continue
            self.assertEqual(struct.unpack_from(">I", image, address - MODULE_BASE)[0], 0x7D8802A6, sub(address))

    def test_generated_sources_define_each_address_once(self):
        for address in HOOKED:
            hits = 0
            for path in glob.glob(os.path.join(GENERATED, "*recomp.*.cpp")):
                hits += read_file(path, "r", encoding="utf-8", errors="replace").count("DEFINE_REX_FUNC(%s) {" % sub(address))
            self.assertEqual(hits, 1, sub(address))

    @unittest.skipUnless(os.path.exists(BF), "TMNT.bf not present")
    def test_script_functions_match_their_addresses(self):
        data = read_file(BF)
        records = {r[1]: r for r in bf.read_records(data)}

        def script(entry, function):
            text = bf.entry_bytes(data, records[entry]).replace(b"\0", b"").decode("latin-1")
            start = re.search(r"function[^\n]*\b%s\s*\(" % function, text)
            self.assertIsNotNone(start, function)
            following = re.search(r"\nfunction[^\n]*\(", text[start.end():])
            return text[start.start():start.end() + following.start()] if following else text[start.start():]

        cases = [
            (0x8759F858, "Leaderboard_Public.fcl", "Leaderboard_SwitchPage", "Leaderboard: "),
            (0x875BA998, "FireCommunicator_Public.fcl", "FireCommunicator_OnReceiveEvent", "Come on - UI_FIRE_REC_LOAD_PL!"),
            (0x87599B80, "InGameHUD_Trigger.fcl", "InGameHUD_GoShowSummary", "No updates for invalid gamer - "),
            (0x875A6770, "SessionWorld_Public.fcl", "SessionWorld_RequestWriteStatsAllValid", "Writing stats for player "),
            (0x876B75C0, "StartMenuWorld_Public.fcl", "StartMenuWorld_CheckSignIn", "Removing invite "),
        ]
        for address, entry, function, marker in cases:
            self.assertIn(marker, script(entry, function), function)
            self.assertIn(marker, function_facts(sub(address))[0], sub(address))

    @unittest.skipUnless(os.path.exists(BF), "TMNT.bf not present")
    def test_leaderboard_send_data_to_fire_matches_its_address(self):
        # Leaderboard_SendDataToFire is a track (.fct), with no "function ... (" header, so it is matched by its
        # one distinctive DBG_TraceString instead of the .fcl helper above.
        data = read_file(BF)
        records = {r[1]: r for r in bf.read_records(data)}
        track = bf.entry_bytes(data, records["Leaderboard_SendDataToFire.fct"]).replace(b"\0", b"").decode("latin-1")
        marker = "Error occured in getting leaderboard result name - "
        self.assertIn(marker, track)
        strings, imports, _ = function_facts("sub_875A11B0")
        self.assertIn(marker, strings)
        self.assertIn(1935, imports)  # PLT_GetRowLongIntByColumnNum
        self.assertIn(1929, imports)  # PLT_GetRowPlayerRank
        self.assertIn(1931, imports)  # PLT_GetRowPlayerName

    @unittest.skipUnless(os.path.exists(BF), "TMNT.bf not present")
    def test_can_view_profile_matches_its_script(self):
        data = read_file(BF)
        records = {r[1]: r for r in bf.read_records(data)}
        text = bf.entry_bytes(data, records["BasicWorld_Public.fcl"]).replace(b"\0", b"").decode("latin-1")
        start = text.index("BasicWorld_CheckIfPlayerCanViewProfile(int i_ControllerID, longint l_PlayerID)")
        script = text[start:text.index("\nfunction", start)]
        self.assertIn("PLT_IsPlayerSignedIntoConsole", script)
        self.assertIn("PLT_C_Privilege_Profile", script)
        self.assertIn("PLT_CheckIsFriend", script)
        _, imports, _ = function_facts("sub_8757E9E0")
        self.assertIn(1875, imports)  # PLT_IsPlayerSignedIntoConsole
        self.assertEqual(imports.count(1890), 2)  # PLT_CheckPlayerPrivilege: Profile, ProfileFriends
        self.assertIn(1910, imports)  # PLT_CheckIsFriend

    def test_platform_wrappers_match_their_structure(self):
        _, imports, calls = function_facts("sub_876AE008")
        self.assertEqual(sorted(set(imports)), [1228, 1876, 1877])
        _, imports, calls = function_facts("sub_876ADF58")
        self.assertEqual(sorted(set(imports)), [1228, 1875, 1877])
        _, imports, calls = function_facts("sub_876AE720")
        self.assertIn("sub_876AE008", calls)
        self.assertIn("sub_876ADF58", calls)
        _, imports, calls = function_facts("sub_876B75C0")
        self.assertIn("sub_876AE008", calls)
        self.assertIn("sub_875BA650", calls)
        _, imports, calls = function_facts("sub_875BA650")
        self.assertIn(1875, imports)
        self.assertEqual(imports.count(1753), 4)


if __name__ == "__main__":
    unittest.main()
