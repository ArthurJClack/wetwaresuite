"""Tests for the Python client. The ones that need the suite run against the
real binary ($ACHCLWSS_BIN, or achclwss on PATH / in ../build) and are
skipped when it can't be found."""

import os
import tempfile
import unittest
from pathlib import Path

import achclwss
from achclwss.client import Result, _decode, _to_arg

PNG = b"\x89PNG\r\n\x1a\n"


def binary():
    try:
        return achclwss.find_binary()
    except achclwss.AchclwssError:
        return None


BIN = binary()
needs_binary = unittest.skipUnless(BIN and os.path.isfile(BIN), "achclwss binary not found (set ACHCLWSS_BIN)")


class Offline(unittest.TestCase):
    def test_result_parsing(self):
        r = Result(
            {
                "content": [
                    {"type": "text", "text": "created img1\nfrom a.png\ndoc: img1\nwidth: 10\nattachment: preview.png (8 bytes)"},
                    {"type": "image", "data": "iVBORw0KGgo=", "mimeType": "image/png"},
                ],
                "isError": False,
                "structuredContent": {"doc": "img1", "width": 10, "items": '[{"path": "a"}]'},
            }
        )
        self.assertTrue(r.ok)
        self.assertEqual(r.doc, "img1")
        self.assertEqual(r["width"], 10)
        self.assertEqual(r.data["items"], [{"path": "a"}])
        self.assertEqual(r.image[:8], PNG)
        self.assertIn("img1", repr(r))
        self.assertEqual(r.message, "created img1\nfrom a.png")

    def test_argument_conversion(self):
        self.assertEqual(_to_arg(Path("a") / "b.png", {"type": "string"}), os.path.join("a", "b.png"))
        self.assertEqual(_to_arg([1, 2, 3], {"type": "array"}), [1.0, 2.0, 3.0])
        self.assertEqual(_to_arg(["a", "b"], {"type": "string"}), "a,b")
        self.assertEqual(_to_arg({"x": 1}, {"type": "string"}), '{"x": 1}')
        self.assertEqual(_decode("[not json"), "[not json")
        self.assertEqual(_decode("plain"), "plain")


@needs_binary
class WithSuite(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.suite = achclwss.Suite(BIN, timeout=60)
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.suite.close()
        cls.tmp.cleanup()

    def test_tools_and_commands(self):
        s = self.suite
        self.assertIn("photo", dir(s))
        self.assertIn("photo.resize", s.commands())
        self.assertIn("resize", dir(s.photo))
        self.assertIn("width", s.photo.resize.__doc__)
        with self.assertRaises(AttributeError):
            s.nosuchtool
        with self.assertRaises(AttributeError):
            s.photo.nosuchcommand
        self.assertNotIn("automate.shell", s.commands())
        self.assertTrue(s.server_info.get("name"))

    def test_edit_preview_save_undo(self):
        s = self.suite
        img = s.photo.new(width=64, height=32, background="#336699").doc
        self.assertTrue(img.startswith("img"))
        r = s.photo.resize(doc=img, width=32, preview=True)
        self.assertEqual(r["height"], 16)
        self.assertEqual(r.image[:8], PNG)
        self.assertTrue(s.undo())
        self.assertEqual(s.photo.info(img)["width"], 64)  # positional fills doc
        self.assertTrue(s.redo())
        out = self.dir / "small.png"
        s.photo.save(doc=img, path=out)
        self.assertTrue(out.is_file())
        self.assertTrue(any(d["id"] == img for d in s.documents()))
        self.assertIn("Resize image", s.history())

    def test_number_lists(self):
        s = self.suite
        art = s.paint.new(width=50, height=50).doc
        r = s.paint.stroke(doc=art, points=[5, 25, 45, 25], size=4, color="black")
        self.assertTrue(r.ok)

    def test_errors(self):
        s = self.suite
        with self.assertRaises(achclwss.CommandError) as ctx:
            s.photo.open(path=str(self.dir / "missing.png"))
        self.assertFalse(ctx.exception.result.ok)
        quiet = achclwss.Suite(BIN, tools=["photo"], raise_errors=False)
        try:
            r = quiet.photo.open(path=str(self.dir / "missing.png"))
            self.assertFalse(r.ok)
            self.assertNotIn("paint", quiet.tools)
        finally:
            quiet.close()

    def test_library_structured_results(self):
        s = self.suite
        assets = self.dir / "assets"
        assets.mkdir()
        img = s.photo.new(width=40, height=20).doc
        s.photo.save(doc=img, path=assets / "a.png")
        s.photo.save(doc=img, path=assets / "b.png")
        s.library.index(root=assets)
        found = s.library.search(query="ext:png", library=assets)
        self.assertEqual(found["total"], 2)
        self.assertEqual([i["name"] for i in found["items"]], ["a.png", "b.png"])
        s.library.tag(query="a.png", tags=["keep", "web"])
        self.assertEqual(s.library.search(query="tag:web")["total"], 1)
        dup = s.library.duplicates()
        self.assertEqual(dup["groups"], 1)
        sheet = s.library.thumbnails(query="", preview=True)
        self.assertEqual(sheet.image[:8], PNG)

    def test_automate_from_python(self):
        s = self.suite
        r = s.automate.run_script(text="photo.new width=$w height=$w -> a\nphoto.save doc=$a path=$out",
                                  vars={"w": 12, "out": str(self.dir / "v.png")})
        self.assertEqual(r["ran"], 2)
        self.assertTrue((self.dir / "v.png").is_file())

    def test_context_manager(self):
        with achclwss.Suite(BIN, tools=["toolkit"]) as s:
            self.assertIn("toolkit.inspect", s.commands())
            proc = s._proc
        self.assertIsNotNone(proc.poll())

    def test_run_batch(self):
        out = self.dir / "batch.png"
        r = achclwss.run_batch(f'photo.new width=10 height=10 -> a\nphoto.save doc=$a path="{out}"\n', binary=BIN)
        self.assertTrue(r.ok and out.is_file())
        self.assertIn("img1", r.output)
        with self.assertRaises(achclwss.AchclwssError):
            achclwss.run_batch("photo.resize doc=nope width=3", binary=BIN)
        failed = achclwss.run_batch("photo.resize doc=nope width=3", binary=BIN, check=False)
        self.assertFalse(failed.ok)
        self.assertIn("line 1", failed.errors)


if __name__ == "__main__":
    unittest.main()
