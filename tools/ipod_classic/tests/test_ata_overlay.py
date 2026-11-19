#!/usr/bin/env python3
"""Verify Classic ATA write persistence with a disposable qcow2 overlay."""
import argparse
from contextlib import contextmanager
from pathlib import Path
import select
import struct
import subprocess
import tempfile
import time

ATA = 0x38700000
DATA, COUNT, LBA0, LBA1, LBA2, DEVICE, STATUS, RDATA = (
    0x54, 0x5C, 0x60, 0x64, 0x68, 0x6C, 0x70, 0x7C
)
BSY, DRQ, ERR = 0x80, 0x08, 0x01
SECTOR = 512
SENTINEL = bytes((index * 17 + 23) % 256 for index in range(SECTOR))

class Controller:
    def __init__(self, proc, logs):
        self.proc, self.logs = proc, logs

    def command(self, command):
        self.proc.stdin.write(command + "\n")
        self.proc.stdin.flush()
        if not select.select([self.proc.stdout], [], [], 5)[0]:
            raise RuntimeError(f"qtest timeout: {command}")
        reply = self.proc.stdout.readline().split()
        if not reply or reply[0] != "OK":
            self.logs.seek(0)
            raise RuntimeError(f"{command}: {reply}; {self.logs.read()[-1200:]}")
        return int(reply[1], 0) if len(reply) == 2 else None

    def write(self, reg, value):
        self.command(f"writel {ATA + reg:#x} {value:#x}")

    def read(self, reg):
        # Reading the PIO register latches the result into the bridge's RDATA.
        self.command(f"readl {ATA + reg:#x}")
        return self.command(f"readl {ATA + RDATA:#x}")

    def wait(self, drq=False):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            status = self.read(STATUS)
            if not status & BSY:
                if status & ERR:
                    raise RuntimeError(f"ATA error, status={status:#x}")
                if bool(status & DRQ) == drq:
                    return
            time.sleep(0.001)
        raise RuntimeError(f"ATA timed out with status={status:#x}, want DRQ={drq}")

    def select_sector(self, lba):
        self.write(DEVICE, 0xE0)
        self.write(COUNT, 1)
        self.write(LBA0, lba & 0xff)
        self.write(LBA1, (lba >> 8) & 0xff)
        self.write(LBA2, (lba >> 16) & 0xff)

    def write_sector(self, lba, data):
        self.select_sector(lba)
        self.write(STATUS, 0x30)  # WRITE SECTORS
        self.wait(drq=True)
        for (word,) in struct.iter_unpack('<H', data):
            self.write(DATA, word)
        self.wait()
        self.write(STATUS, 0xE7)  # FLUSH CACHE
        self.wait()

    def read_sector(self, lba):
        self.select_sector(lba)
        self.write(STATUS, 0x20)  # READ SECTORS
        self.wait(drq=True)
        return b''.join(struct.pack('<H', self.read(DATA) & 0xffff)
                        for _ in range(SECTOR // 2))

@contextmanager
def emulator(qemu, bios, overlay):
    command = [qemu, '-machine', 'iPod-Classic,accel=qtest', '-display', 'none',
               '-qtest', 'stdio', '-qtest-log', 'none', '-nodefaults', '-L', bios,
               '-drive', f'if=ide,format=qcow2,file={overlay}']
    with tempfile.TemporaryFile(mode='w+t') as logs:
        with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=logs, text=True) as proc:
            try:
                yield Controller(proc, logs)
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()

def test(qemu, image_tool, bios):
    # Only temporary regular files are accepted; no external drive argument.
    with tempfile.TemporaryDirectory(prefix='ipod-ata-overlay-') as work:
        base = Path(work, 'seed.raw')
        overlay = Path(work, 'persist.qcow2')
        original = bytes(SECTOR * 4096)
        base.write_bytes(original)
        base.chmod(0o444)
        subprocess.run([image_tool, 'create', '-q', '-f', 'qcow2',
                        '-F', 'raw', '-b', str(base), str(overlay)], check=True)

        with emulator(qemu, bios, overlay) as disk:
            if disk.read_sector(17) != bytes(SECTOR):
                raise RuntimeError('Unexpected data in fresh disk')
            disk.write_sector(17, SENTINEL)
            if disk.read_sector(17) != SENTINEL:
                raise RuntimeError('Immediate ATA readback differed')

        with emulator(qemu, bios, overlay) as disk:
            if disk.read_sector(17) != SENTINEL:
                raise RuntimeError('ATA readback differed after emulator restart')
            if disk.read_sector(18) != bytes(SECTOR):
                raise RuntimeError('Adjacent sector changed unexpectedly')

        if base.read_bytes() != original:
            raise RuntimeError('Read-only raw seed changed')
        print('PASS: ATA write and FLUSH survived restart; adjacent sector and raw seed unchanged')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('qemu')
    parser.add_argument('qemu_img')
    parser.add_argument('--bios-dir', required=True)
    args = parser.parse_args()
    test(args.qemu, args.qemu_img, args.bios_dir)
