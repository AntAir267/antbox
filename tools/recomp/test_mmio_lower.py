"""mmio_lower: constant device-register accesses to MMIO_RD/MMIO_WR (BO-72)."""
import unittest

from .mmio_lower import lower_stmt, literal_value


class MmioLowerTest(unittest.TestCase):
    def test_literal_forms(self):
        self.assertEqual(literal_value("0xFE820010u"), 0xFE820010)
        self.assertEqual(literal_value("-25034736"), 0xFE820010)

    def test_load(self):
        self.assertEqual(lower_stmt("eax = MEM32(0xFE820010u);"),
                         ("eax = MMIO_RD32(0xFE820010u);", 1))
        self.assertEqual(lower_stmt("ecx = MEM32(-25034736);"),
                         ("ecx = MMIO_RD32(-25034736);", 1))

    def test_store(self):
        self.assertEqual(lower_stmt("MEM32(-25033992) = ecx;"),
                         ("MMIO_WR32(-25033992, ecx);", 1))
        self.assertEqual(lower_stmt("MEM32(0xFE8202FCu) = eax;"),
                         ("MMIO_WR32(0xFE8202FCu, eax);", 1))
        self.assertEqual(lower_stmt("MEM8(0xFED00004u) = LO8(eax);"),
                         ("MMIO_WR8(0xFED00004u, LO8(eax));", 1))

    def test_test_line_keeps_comment(self):
        s = ("_fa = (uint32_t)((MEM8(0xFE820010u)) & (0x10)) & 0xFFu; "
             "_fb = (uint32_t)(0) & 0xFFu; /* test MEM8(0xFE820010u), 0x10 */")
        out, n = lower_stmt(s)
        self.assertEqual(n, 1)
        self.assertIn("MMIO_RD8(0xFE820010u)", out)
        self.assertIn("/* test MEM8(0xFE820010u), 0x10 */", out)

    def test_outside_window_untouched(self):
        for s in ("eax = MEM32(0x300C28);", "MEM32(ebp + 8) = 0xFE820400u;",
                  "eax = MEM32(ecx + 0xFE820010u);",
                  "eax = MEM32(0xFEC0012Cu);"):
            self.assertEqual(lower_stmt(s), (s, 0))

    def test_other_lvalue_uses_untouched(self):
        for s in ("MEM32(0xFE820010u) |= eax;",
                  "MEM32(0xFE820010u)++;",
                  "if (_cf) MEM32(0xFE820010u) = eax; /* cmovb */"):
            self.assertEqual(lower_stmt(s), (s, 0))

    def test_read_modify_write(self):
        self.assertEqual(
            lower_stmt("MEM32(0xFE820010u) = MEM32(0xFE820010u) + eax;"),
            ("MMIO_WR32(0xFE820010u, MMIO_RD32(0xFE820010u) + eax);", 2))


if __name__ == "__main__":
    unittest.main()
