"""Real 0xC1 payloads captured 2026-10-18 12:30-12:36 (SlyTherm #221)."""
import unittest

from slylog_collector.furnace_menu import decode_menu_reply

MENU = bytes.fromhex(
    '0100000000f0a0a6b6c5d05354415455532031b7b6c5d05354415455532032b7b632574b20484953'
    '54b7b64c494645202048495354b7b64641554c542048495354b7b6554e495420494e464fb7b65345'
    '545550b7')
IDLE = bytes.fromhex(
    '0101000000f0b6c5d05354415455532031b7c2e24d41494e204c494d4954e04e412053544e444259'
    'c3c2e24d524c4320494e505554e04e412053544e444259c3c2e248414c4320494e505554e04e4120'
    '53544e444259c3c2e2415050524f582049444d2052504de0204f4646c3c2e24655524e204c4f2050'
    '52205357e04f50454e202020c3c2e24655524e204849205052205357e04f50454e202020c3c2e247'
    '415320564c56205052434e54e020203025c3c2e247415320564c562052454c4159e04f50454e2020'
    '20c3c2e2464c414d45e04f464620202020202020c3c2e2424c4f5745522043464de020202030c3')
HEAT = bytes.fromhex(
    '0101000000f0b6c5d05354415455532031b7c2e24d41494e204c494d4954e0434c4f534544202020'
    'c3c2e24d524c4320494e505554e0434c4f534544202020c3c2e248414c4320494e505554e0434c4f'
    '534544202020c3c2e2415050524f582049444d2052504de032373230c3c2e24655524e204c4f2050'
    '52205357e0434c4f53454420c3c2e24655524e204849205052205357e0434c4f53454420c3c2e247'
    '415320564c56205052434e54e031303025c3c2e247415320564c562052454c4159e0434c4f534544'
    '20c3c2e2464c414d45e0474f4f44202020202020c3c2e2424c4f5745522043464de031363832c3')


class FurnaceMenuTest(unittest.TestCase):
    def test_menu_list(self):
        m = decode_menu_reply(MENU)
        self.assertEqual(m.page, 0)
        self.assertEqual(m.menu_titles, ["STATUS 1", "STATUS 2", "2WK HIST",
                                         "LIFE  HIST", "FAULT HIST", "UNIT INFO", "SETUP"])
        self.assertTrue(m.complete)

    def test_status1_idle(self):
        m = decode_menu_reply(IDLE)
        self.assertEqual((m.page, m.title, m.complete), (1, "STATUS 1", True))
        rows = {l: (v, n) for l, v, n in m.rows}
        self.assertEqual(len(m.rows), 10)
        self.assertEqual(rows["GAS VLV PRCNT"], ("0%", 0.0))
        self.assertEqual(rows["FLAME"][0], "OFF")
        self.assertEqual(rows["BLOWER CFM"], ("0", 0.0))
        self.assertEqual(rows["MAIN LIMIT"][0], "NA STNDBY")

    def test_status1_heat_call(self):
        m = decode_menu_reply(HEAT)
        rows = {l: (v, n) for l, v, n in m.rows}
        self.assertEqual(rows["GAS VLV PRCNT"], ("100%", 100.0))
        self.assertEqual(rows["FLAME"][0], "GOOD")
        self.assertEqual(rows["BLOWER CFM"], ("1682", 1682.0))
        self.assertEqual(rows["APPROX IDM RPM"], ("2720", 2720.0))
        self.assertEqual(rows["GAS VLV RELAY"][0], "CLOSED")
        self.assertIsNone(rows["FURN LO PR SW"][1])

    def test_row_order_is_page_order(self):
        m = decode_menu_reply(HEAT)
        self.assertEqual([r[0] for r in m.rows][:2], ["MAIN LIMIT", "MRLC INPUT"])
        self.assertEqual(m.rows[-1][0], "BLOWER CFM")

    def test_truncated_page_is_kept_incomplete(self):
        m = decode_menu_reply(HEAT[:120])
        self.assertFalse(m.complete)
        self.assertEqual(m.payload, HEAT[:120])
        self.assertGreater(len(m.rows), 0)

    def test_not_a_menu_page(self):
        self.assertIsNone(decode_menu_reply(bytes.fromhex("0600000c0162")))
        self.assertIsNone(decode_menu_reply(b""))


if __name__ == "__main__":
    unittest.main()
