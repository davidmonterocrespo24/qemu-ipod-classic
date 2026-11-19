#!/usr/bin/env python3
"""Build an iPod Classic 6G style disk image (INVESTIGATION SCRATCH TOOL).

    mkdisk6g.py <out> <firmware-partition-image> <fat32-image> <fw-start-lba>

Lays out: MBR (partition 1 = type 0x00 firmware, partition 2 = type 0x0B
FAT32), the firmware partition image, then the FAT32 image.  Everything is in
512-byte sectors.  See wiki/11 for why the block size matters.
"""
import struct
import sys
import os

out, fwp, fat, start = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
fwb = open(fwp, 'rb').read()
fatb = open(fat, 'rb').read()
sec = 512
# The partition table counts complete sectors. Pad both payloads so their
# physical offsets and lengths agree with the LBAs and sizes in the MBR.
fwb += b'\0' * (-len(fwb) % sec)
fatb += b'\0' * (-len(fatb) % sec)
fw_sectors = (len(fwb) + sec - 1) // sec
fat_start = start + fw_sectors
fat_sectors = len(fatb) // sec

mbr = bytearray(512)


def ent(off, ptype, lba, cnt):
    mbr[off + 4] = ptype
    mbr[off + 8:off + 12] = struct.pack('<I', lba)
    mbr[off + 12:off + 16] = struct.pack('<I', cnt)


ent(0x1be, 0x00, start, fw_sectors)
ent(0x1ce, 0x0b, fat_start, fat_sectors)
mbr[0x1fe:0x200] = b'\x55\xaa'

with open(out, 'wb') as f:
    f.write(bytes(mbr))
    f.write(b'\0' * (sec * (start - 1)))
    f.write(fwb)
    f.write(fatb)

print("fw: lba %d, %d sectors; fat: lba %d, %d sectors; total %d MB" %
      (start, fw_sectors, fat_start, fat_sectors,
       os.path.getsize(out) // (1024 * 1024)))
