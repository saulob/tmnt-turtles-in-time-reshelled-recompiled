import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import bf_extract as bf

BF_PATH = os.path.join(os.path.dirname(__file__), "..", "game", "extracted", "Data", "TMNT.bf")


class Lzo1xTests(unittest.TestCase):
    def test_literals_match_and_end_marker(self):
        stream = bytes([22]) + b"abcde" + bytes([0x70, 0x00]) + bytes([0x11, 0x00, 0x00])
        self.assertEqual(bf.lzo1x_decompress(stream), b"abcdeabcd")

    def test_extended_literal_run(self):
        stream = bytes([0, 5]) + b"x" * 23 + bytes([0x11, 0x00, 0x00])
        self.assertEqual(bf.lzo1x_decompress(stream), b"x" * 23)


@unittest.skipUnless(os.path.exists(BF_PATH), "TMNT.bf not present")
class ArchiveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(BF_PATH, "rb") as f:
            cls.data = f.read()
        cls.records = {r[1]: r for r in bf.read_records(cls.data)}

    def entry(self, name):
        import struct
        _, _, size, _, offset, _ = self.records[name]
        payload = self.data[offset:offset + size]
        csize, usize = struct.unpack_from("<2I", payload, 0)
        body = payload[bf.PAYLOAD_HEADER:bf.PAYLOAD_HEADER + csize]
        return bf.lzo1x_decompress(body) if csize < usize else body, usize

    def test_session_manager_entries_decode_to_declared_size(self):
        for suffix, size in ((".fct.pp", 2812), (".fct", 2300), (".efc", 3420)):
            text, usize = self.entry("02_SessionManager_CreateLocalSession" + suffix)
            self.assertEqual(len(text), size)
            self.assertEqual(usize, size)

    def test_create_local_session_source(self):
        text, _ = self.entry("02_SessionManager_CreateLocalSession.fct")
        self.assertIn(b"LOC_NOTF_LEADERBRD_NOT_XLA", text)
        self.assertIn(b"mi_Handle = PLT_CreatePlatformSession", text)

    def test_notification_id_definition(self):
        text, _ = self.entry("Localization_Defs.h")
        self.assertRegex(text, rb"LOC_NOTF_LEADERBRD_NOT_XLA\s+16")

    def test_full_account_online_call_sites(self):
        sites = bf.native_call_sites(self.data, "PLT_IsSignedIntoFullAccountOnline")
        counts = {}
        for _, function, _, _ in sites:
            counts[function] = counts.get(function, 0) + 1
        self.assertEqual(len(sites), 17)
        self.assertEqual(counts["Leaderboard_SwitchPage"], 1)
        self.assertEqual(counts["FireCommunicator_OnReceiveEvent"], 6)
        for function in ("SessionManager_CreateLocalSession", "SessionManager_JoinSession",
                         "SessionManager_QuickMatch", "SessionManager_JoinByInvite",
                         "SessionManager_Matchmake", "SessionManager_CheckIfCanPlayOnline",
                         "SessionManager_SendInvites", "StartMenuWorld_CheckSignIn",
                         "SessionWorld_CheckSignIn"):
            self.assertEqual(counts[function], 1)


if __name__ == "__main__":
    unittest.main()
