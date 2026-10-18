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

PAGE2 = bytes.fromhex(
    '0102000000f0b6c5d05354415455532032b7c2e2'
    '4d4f4445e04f46462020202020c3c2e24d4f544f'
    '52204d464752e05247424c542020c3c2e24d4f54'
    '4f522052504de020202030c3c2e24d4158494d55'
    '4d2043464de031393030c3c2e2424c4f57455220'
    '43464de020202030c3c2e248554d204f55545055'
    '54e0204f4646c3c2e2454143204f5554505554e0'
    '204f4646c3')
PAGE3 = bytes.fromhex(
    '0103000000f0b632574b2048495354b7c2e24d4f'
    '4420485420485253e0202020202033c3c2e24d4f'
    '44204854204359434c53e0202020203231c3c2e2'
    '424c4f57455220485253e0202020203139c3c2e2'
    '424c4f574552204359434c53e0202020203934c3')
PAGE4 = bytes.fromhex(
    '0104000000f0b64c494645202048495354b7c2e2'
    '544f54414c20444159532050575244e020202031'
    '3231c3c2e24d4f4420485420485253e020202020'
    '2034c3c2e24d4f44204854204359434c4553e020'
    '2020203331c3c2e2424c4f57455220485253e020'
    '2031303439c3c2e2424c4f574552204359434c53'
    'e0202032393834c3')
PAGE5 = bytes.fromhex(
    '0105000000f0b64641554c542048495354b7c2e2'
    '524556455253454420504c5459202020e0444159'
    '53203638c3c2e24c5043204f50454e2020202020'
    '202020e044415953203933c3c2e2485043204f50'
    '454e2020202020202020e044415953203933c3c2'
    'e2524556455253454420504c5459202020e04441'
    '5953203936c3c2e24e4f204641554c5420202020'
    '20202020e044415953202030c3c2e24e4f204641'
    '554c542020202020202020e044415953202030c3'
    'c2e1434c454152204641554c5453e84e4f20e059'
    '4553c3')
PAGE6 = bytes.fromhex(
    '0106000000f0b6554e495420494e464fb7c2e24d'
    '4ee044455454534f4e203130354b205633202020'
    '2020202020c3c2e2534ee04e4f4e452041564149'
    '4c41424c45202020202020202020c3c2e2534f46'
    '54574152452056455253e0363030333739303120'
    '20c3')
PAGE7 = bytes.fromhex(
    '0107000000f0b65345545550b7c2e14845415452'
    '4953452041444a555354e0353546e0363046c3c2'
    'e14d494e20484541542041444a25e030e037e031'
    '35e02d3135e02d37c3c2e14d4158204845415420'
    '41444a25e030e037e03135e02d3135e02d37c3c2'
    'e1464958454420464952452052415445e04f4646'
    'e0343025e0353025e0363025e0373025e0373525'
    'e0383025e0383525e0393025e031303025c3c2e1'
    '524553455420414c4c2044464c5453e84e4fe059'
    '4553c3')


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
        rows = {r.label: (r.value, r.number) for r in m.rows}
        self.assertEqual(len(m.rows), 10)
        self.assertEqual(rows["GAS VLV PRCNT"], ("0%", 0.0))
        self.assertEqual(rows["FLAME"][0], "OFF")
        self.assertEqual(rows["BLOWER CFM"], ("0", 0.0))
        self.assertEqual(rows["MAIN LIMIT"][0], "NA STNDBY")

    def test_status1_heat_call(self):
        m = decode_menu_reply(HEAT)
        rows = {r.label: (r.value, r.number) for r in m.rows}
        self.assertEqual(rows["GAS VLV PRCNT"], ("100%", 100.0))
        self.assertEqual(rows["FLAME"][0], "GOOD")
        self.assertEqual(rows["BLOWER CFM"], ("1682", 1682.0))
        self.assertEqual(rows["APPROX IDM RPM"], ("2720", 2720.0))
        self.assertEqual(rows["GAS VLV RELAY"][0], "CLOSED")
        self.assertIsNone(rows["FURN LO PR SW"][1])

    def test_row_order_is_page_order(self):
        m = decode_menu_reply(HEAT)
        self.assertEqual([r.label for r in m.rows][:2], ["MAIN LIMIT", "MRLC INPUT"])
        self.assertEqual(m.rows[-1].label, "BLOWER CFM")

    def test_truncated_page_is_kept_incomplete(self):
        m = decode_menu_reply(HEAT[:120])
        self.assertFalse(m.complete)
        self.assertEqual(m.payload, HEAT[:120])
        self.assertGreater(len(m.rows), 0)

    def test_not_a_menu_page(self):
        self.assertIsNone(decode_menu_reply(bytes.fromhex("0600000c0162")))
        self.assertIsNone(decode_menu_reply(b""))

    def test_status2(self):
        m = decode_menu_reply(PAGE2)
        rows = {r.label: r for r in m.rows}
        self.assertEqual((m.title, m.complete), ("STATUS 2", True))
        self.assertEqual(rows["MAXIMUM CFM"].number, 1900.0)
        self.assertEqual(rows["MODE"].value, "OFF")

    def test_history_pages_have_titles_without_marker(self):
        for n, title in ((3, "2WK HIST"), (4, "LIFE  HIST"), (6, "UNIT INFO")):
            m = decode_menu_reply(globals()[f"PAGE{n}"])
            self.assertEqual((m.title, m.complete), (title, True), n)
        life = {r.label: r.number for r in decode_menu_reply(PAGE4).rows}
        self.assertEqual(life["BLOWER HRS"], 1049.0)
        unit = {r.label: r.value for r in decode_menu_reply(PAGE6).rows}
        self.assertEqual(unit["MN"], "DETTSON 105K V3")
        self.assertEqual(unit["SOFTWARE VERS"], "60037901")

    def test_fault_history_days_and_selector(self):
        m = decode_menu_reply(PAGE5)
        self.assertTrue(m.complete)
        self.assertEqual((m.rows[0].label, m.rows[0].value, m.rows[0].number),
                         ("REVERSED PLTY", "DAYS 68", 68.0))
        self.assertEqual(m.rows[-1].label, "CLEAR FAULTS")
        self.assertEqual((m.rows[-1].value, m.rows[-1].options), ("NO", "YES"))

    def test_setup_editable_rows(self):
        m = decode_menu_reply(PAGE7)
        self.assertTrue(m.complete)
        rows = {r.label: r for r in m.rows}
        self.assertEqual((rows["HEATRISE ADJUST"].value, rows["HEATRISE ADJUST"].options),
                         ("55F", "60F"))
        self.assertEqual(rows["MIN HEAT ADJ%"].value, "0")
        self.assertEqual(rows["MIN HEAT ADJ%"].options, "7|15|-15|-7")
        self.assertEqual(rows["FIXED FIRE RATE"].value, "OFF")
        self.assertTrue(rows["FIXED FIRE RATE"].options.endswith("100%"))
        self.assertEqual(rows["RESET ALL DFLTS"].options, "YES")


if __name__ == "__main__":
    unittest.main()
