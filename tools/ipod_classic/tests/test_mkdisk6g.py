#!/usr/bin/env python3
"""Exercise the image builder with synthetic payloads, never real firmware.

Run directly with Python. MKDISK6G_SCRIPT may name a baseline copy
for A/B tests.
"""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(os.environ.get(
    'MKDISK6G_SCRIPT', Path(__file__).resolve().parents[1] / 'mkdisk6g.py'))
SECTOR = 512


class DiskLayoutTest(unittest.TestCase):
    def check_layout(self, firmware_size, fat_size, start=8):
        # Distinct deterministic payloads make an overlap or shift observable.
        firmware = bytes((i * 7 + 3) % 256 for i in range(firmware_size))
        fat = bytes((i * 11 + 5) % 256 for i in range(fat_size))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fw_path, fat_path, image_path = [root / n for n in
                                             ('firmware', 'fat', 'disk.img')]
            fw_path.write_bytes(firmware)
            fat_path.write_bytes(fat)
            subprocess.run([sys.executable, str(SCRIPT), str(image_path),
                            str(fw_path), str(fat_path), str(start)],
                           check=True, capture_output=True)
            image = image_path.read_bytes()
            self.assertEqual(image[510:512], b'\x55\xaa')
            self.assertEqual(image[446 + 4], 0x00)
            self.assertEqual(image[462 + 4], 0x0b)
            fw_lba, fw_count = struct.unpack_from('<II', image, 446 + 8)
            fat_lba, fat_count = struct.unpack_from('<II', image, 462 + 8)
            self.assertEqual(fw_lba, start)
            self.assertEqual(fw_count, (len(firmware) + SECTOR - 1) // SECTOR)
            self.assertEqual(fat_lba, start + fw_count)
            self.assertEqual(fat_count, (len(fat) + SECTOR - 1) // SECTOR)
            self.assertEqual(len(image), (fat_lba + fat_count) * SECTOR)
            self.assertEqual(image[SECTOR:start * SECTOR],
                             bytes((start - 1) * SECTOR))
            self.assertEqual(image[fw_lba * SECTOR:fat_lba * SECTOR],
                             firmware + bytes(-len(firmware) % SECTOR))
            self.assertEqual(image[fat_lba * SECTOR:],
                             fat + bytes(-len(fat) % SECTOR))
            self.assertEqual(fw_path.read_bytes(), firmware)
            self.assertEqual(fat_path.read_bytes(), fat)

    def test_aligned(self):
        self.check_layout(512, 1024)

    def test_short_firmware(self):
        self.check_layout(511, 1024)

    def test_long_firmware(self):
        self.check_layout(513, 1024)

    def test_short_fat(self):
        self.check_layout(512, 511)

    def test_long_fat(self):
        self.check_layout(512, 513)

    def test_both_unaligned(self):
        self.check_layout(513, 1025)

    def test_single_bytes(self):
        self.check_layout(1, 1)

    def test_start_after_mbr(self):
        self.check_layout(513, 513, start=1)

    def test_aligned_large(self):
        self.check_layout(4096, 8192, start=16)


if __name__ == '__main__':
    unittest.main()
