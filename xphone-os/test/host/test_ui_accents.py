"""Check real UI font pixels, measurements, and glass-twin parity on both panels."""
from pathlib import Path
import hashlib
import importlib.util
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]
OS = ROOT / "firmware/xphone-os"
MODES = ("plain", "center", "scaled", "scaled_center", "tail", "wrap")
FONTS = ("regular", "bold", "small")
spec = importlib.util.spec_from_file_location("xpgfx", ROOT / "tools/glass-twin/xpgfx.py")
xpgfx = importlib.util.module_from_spec(spec)
spec.loader.exec_module(xpgfx)


class UiAccentsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.executables = {}
        for device in ("x3", "x4"):
            exe = Path(cls.tmp.name) / device
            flags = ["-DFLOWE_TEST_DISPLAY_X3=1"] if device == "x3" else []
            subprocess.run(["c++", "-std=c++17", "-fsanitize=address,undefined", "-g", *flags,
                            *["-I" + str(OS / p) for p in
                              ("src", "test/host/sync_stubs", "lib/EpdFontCore", "lib/Utf8")],
                            *[str(OS / p) for p in ("src/Gfx.cpp", "src/Fonts.cpp", "lib/Utf8/Utf8.cpp",
                                                   "test/host/ui_accents_gfx_test.cpp")],
                            "-o", str(exe)], check=True)
            cls.executables[device] = exe

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def render(self, device, font, mode, text):
        output = subprocess.check_output([str(self.executables[device]), font, mode, text])
        metrics, ink = output.split(b"\n", 1)
        return tuple(map(int, metrics.split())), ink

    def test_decomposed_equals_precomposed(self):
        pairs = [("abra\u0301zame fuerte", "abrázame fuerte"),
                 ("Cafe\u0301 A\u030angstro\u0308m nin\u0303o", "Café Ångström niño"),
                 ("a\u0301" * 200 + " FIN", "á" * 200 + " FIN")]
        for device in self.executables:
            for font in FONTS:
                for mode in MODES:
                    for decomposed, precomposed in pairs:
                        with self.subTest(device=device, font=font, mode=mode, length=len(decomposed)):
                            actual = self.render(device, font, mode, decomposed)
                            expected = self.render(device, font, mode, precomposed)
                            self.assertEqual(actual[0], expected[0])
                            self.assertEqual(actual[1], expected[1])
                            self.assertEqual(actual[0][0], 1)
                            self.assertIn(1, actual[1])

    def test_scale_one_matches_plain_text(self):
        for device in self.executables:
            for font_name in FONTS:
                for text in ("ASCII 123", "abra\u0301zame fuerte", "a\u0301" * 200):
                    with self.subTest(device=device, font=font_name, text=text[:20]):
                        scaled = self.render(device, font_name, "scale_one", text)
                        self.assertEqual(scaled, self.render(device, font_name, "plain", text))
                        g = xpgfx.Gfx(device=device)
                        g.draw_text_scaled(xpgfx.font(font_name), 11, 13, text, 1)
                        self.assertEqual(scaled[1], bytes(g.ink))

    def test_ascii_unchanged(self):
        # Hashes captured from Gfx before the accent fix; all three actual fonts and six paths.
        expected = {"x3": "b9ba992f383361ae59e6ec58f61c52c0917115ff91d94f21f3df11f6a443e5a2", "x4": "af3ca836027a3ad56934cfe249a6a2baedcc423858b347f7a303376332b160d3"}
        for device in self.executables:
            digest = hashlib.sha256()
            for font in FONTS:
                for mode in MODES:
                    metrics, ink = self.render(device, font, mode, "Opening book 123 - plain ASCII text.")
                    digest.update(str(metrics).encode())
                    digest.update(ink)
            self.assertEqual(digest.hexdigest(), expected[device], device)

    def test_host_renderer_matches_firmware(self):
        for device in self.executables:
            for font_name in FONTS:
                font = xpgfx.font(font_name)
                for mode in MODES:
                    # Unsupported marks must keep the firmware fallback; full Unicode NFC differs.
                    for text in ("abra\u0301zame fuerte", "a\u0301" * 200 + " FIN", "α\u0301 q\u0301 A\u0302\u0301"):
                        with self.subTest(device=device, font=font_name, mode=mode, length=len(text)):
                            g = xpgfx.Gfx(device=device)
                            metrics, ink = self.render(device, font_name, mode, text)
                            self.assertEqual(metrics[1], g.text_width(font, text))
                            if mode == "plain": g.draw_text(font, 11, 13, text)
                            elif mode == "center": g.draw_text_centered(font, 230, 13, text)
                            elif mode == "scaled": g.draw_text_scaled(font, 11, 13, text, 2)
                            elif mode == "scaled_center": g.draw_text_scaled_centered(font, 230, 13, text, 2)
                            elif mode == "tail": g.draw_text(font, 450 - g.text_width(font, text), 13, text)
                            elif mode == "wrap":
                                self.assertEqual(metrics[3], g.draw_text_wrapped(font, 11, 13, text, 143, 8))
                            self.assertEqual(ink, bytes(g.ink))


if __name__ == "__main__":
    unittest.main()
