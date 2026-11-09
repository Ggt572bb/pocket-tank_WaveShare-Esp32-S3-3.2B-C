"""Offline checks for the recovery-image binding used before first install."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from backup_lcd35b import SIZE, verify_backup


class BackupBinding(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.image = Path(self.tmp.name) / "factory.bin"
        with self.image.open("wb") as f:
            f.truncate(SIZE)
        self.meta = {"usb_serial": "TEST-BOARD-A", "size": SIZE,
                     "sha256": hashlib.sha256(self.image.read_bytes()).hexdigest()}
        Path(str(self.image) + ".json").write_text(json.dumps(self.meta))

    def test_matching_image_and_device(self):
        self.assertEqual(verify_backup(self.image, "test-board-a"), self.meta)

    def test_wrong_device(self):
        with self.assertRaisesRegex(ValueError, "another USB"):
            verify_backup(self.image, "TEST-BOARD-B")

    def test_modified_image(self):
        with self.image.open("r+b") as f:
            f.write(b"changed")
        with self.assertRaisesRegex(ValueError, "checksum"):
            verify_backup(self.image, "TEST-BOARD-A")

    def test_truncated_image(self):
        self.image.write_bytes(b"partial")
        with self.assertRaisesRegex(ValueError, "16 MB"):
            verify_backup(self.image, "TEST-BOARD-A")


if __name__ == "__main__":
    unittest.main()
