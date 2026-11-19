#!/usr/bin/env python3
"""Firmware-free QEMU qtest: I2S TX FIFO to the live WAV audio backend."""
import argparse
import os
from pathlib import Path
import select
import struct
import subprocess
import tempfile
import wave

FIFO = 0x3CA00010
QUEUE_BYTES = 1 << 20
FILL_WORD = 0x20001000
LEFT = 0x1111
RIGHT = 0x2222
TAIL = 0x44443333
FILL_FRAMES = QUEUE_BYTES // 4

def test(qemu, bios_dir):
    with tempfile.TemporaryDirectory(prefix='ipod-i2s-test-') as tmp:
        target = Path(tmp, 'live-output.wav')
        args = [qemu, '-machine', 'iPod-Classic,accel=qtest', '-nodefaults',
                '-display', 'none', '-qtest', 'stdio', '-qtest-log', 'none',
                '-audio', f'wav,path={target}', '-L', bios_dir]
        with tempfile.TemporaryFile(mode='w+t') as errors:
            env = os.environ.copy()
            env.pop('AUDIOWAV', None)
            env['AUDIOLIVE'] = '1'
            with subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=errors, text=True, env=env) as p:
                def cmd(command):
                    p.stdin.write(command + '\n')
                    p.stdin.flush()
                    if not select.select([p.stdout], [], [], 10)[0]:
                        raise RuntimeError(f'Timed out: {command}')
                    reply = p.stdout.readline()
                    if not reply.startswith('OK'):
                        errors.seek(0)
                        raise RuntimeError(f'{command}: {reply!r}\n{errors.read()[-1000:]}')
                try:
                    # Fill the ring without advancing the audio backend clock.
                    for _ in range(FILL_FRAMES):
                        cmd(f'writel {FIFO:#x} {FILL_WORD:#x}')
                    cmd(f'writew {FIFO:#x} {LEFT:#x}')
                    # Give the mixer space, then deliver the other half and
                    # known stereo markers. A lost LEFT shifts the markers.
                    for _ in range(25):
                        cmd('clock_step 10000000')
                    cmd(f'writew {FIFO:#x} {RIGHT:#x}')
                    for _ in range(8):
                        cmd(f'writel {FIFO:#x} {TAIL:#x}')
                    for _ in range(700):
                        cmd('clock_step 10000000')
                finally:
                    p.terminate()
                    try:
                        p.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        p.kill()
                        p.wait()
            with wave.open(str(target), 'rb') as wav:
                if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (2, 2, 44100):
                    raise RuntimeError(f'Unexpected WAV format: {wav.getparams()}')
                frames = list(struct.iter_unpack('<HH', wav.readframes(wav.getnframes())))
            fill_pair = (FILL_WORD & 0xffff, FILL_WORD >> 16)
            tail_pair = (TAIL & 0xffff, TAIL >> 16)
            # Ignore the mixer silence so a different host timer cadence
            # cannot be mistaken for a channel-alignment failure.
            final = [f for f in frames if f not in (fill_pair, (0, 0))]
            expected = [(LEFT, RIGHT)] + [tail_pair] * 8
            print('Expected stereo frames:', [tuple(hex(x) for x in f) for f in expected])
            print('Observed stereo frames:', [tuple(hex(x) for x in f) for f in final[:20]])
            if final != expected:
                print('FAIL: audio lost a stereo frame boundary')
                return 1
            print('PASS: all stereo frames match in the live WAV backend')
            return 0

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('qemu')
    parser.add_argument('--bios-dir', required=True)
    args = parser.parse_args()
    raise SystemExit(test(args.qemu, args.bios_dir))
