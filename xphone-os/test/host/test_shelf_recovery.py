#!/usr/bin/env python3
"""Exercise the real index load/save/scan and FBPK extraction on a host SD.

Like test_reader_progress.py, extract complete production methods to avoid
linking the display/BLE scene. Directory I/O and unrelated EPUB/stats are
stubbed. FbpBook.cpp is compiled in full. All fixtures are generated.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

OS = Path(__file__).resolve().parents[2]

class ShelfRecoveryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        out = Path(cls.tmp.name)
        source = (OS / "src/scenes/ReaderScene.cpp").read_text()
        header = (OS / "src/scenes/ReaderScene.h").read_text()
        harness = (OS / "test/host/shelf_recovery_test.cpp").read_text()
        start = source.index("namespace {\n// The shelf index:")
        end = source.index("\n// #26: _totalBooks", start)
        dims = source[source.index("bool readThumbDims("):source.index("\n// F6 (2026-08-19)")]
        entries = header[header.index("  enum class TileMeta"):header.index("\n  // Shelf index")]
        cpp = out / "shelf.cpp"
        cpp.write_text(harness.replace("// PRODUCTION_ENTRIES", entries)
                       .replace("// PRODUCTION_METHODS", dims + source[start:end]))
        flags = ["-O1", "-g", "-fsanitize=address,undefined"]
        includes = ["-I" + str(OS / p) for p in (
            "test/host/shelf_stubs", "test/host/compact_stubs", "test/host/sync_stubs",
            "src", "lib/uzlib/src", "lib/EpdFontCore", "lib/FbpCompact")]
        subprocess.run(["cc", *flags, "-c", str(OS / "lib/uzlib/src/tinflate.c"),
                        "-o", str(out / "tinflate.o")], check=True)
        cls.exe = out / "shelf-test"
        subprocess.run(["c++", "-std=c++17", *flags, *includes,
                        str(OS / "src/reader/FbpBook.cpp"), str(cpp),
                        str(out / "tinflate.o"), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_shelf_recovery(self):
        cases = ("negative", "positive-missing", "positive-truncated", "coverless",
                 "strip-only", "write-failure", "partial-write", "truncated-package",
                 "unknown", "deferred", "cache-hit", "fresh", "bad-dimensions", "declares")
        for case in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as sd:
                subprocess.run([str(self.exe), sd, case], check=True)

if __name__ == "__main__":
    unittest.main()
