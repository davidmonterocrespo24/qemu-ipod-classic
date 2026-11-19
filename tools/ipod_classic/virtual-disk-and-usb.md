# Classic virtual disks and USB

## Build a disk for upstream Classic 6G

Run these commands from the repository root in a shell with Python 3, dosfstools
and the QEMU build tools. Use your own extracted firmware-partition image and
decrypted retailOS payload, following the extraction instructions in the
[project README](../../README.md#running-the-ipod-classic-6g). None of the
commands below need a physical drive path or format a physical disk.

Set these two variables to the respective files from your own firmware dump:

```sh
firmware_partition=/path/to/extracted/firmware-partition.img
retailos=/path/to/decrypted/osos.bin
```

Build a raw seed with an empty FAT32 data volume, then create a writable
QCOW2 overlay:

```sh
work=$(mktemp -d "$PWD/classic6g-disk.XXXXXX")
truncate -s 64M "$work/data.fat"
mkfs.vfat -F 32 -n IPOD "$work/data.fat"
python3 tools/ipod_classic/mkdisk6g4k.py \
    "$work/seed.raw" "$firmware_partition" "$work/data.fat"
chmod a-w "$work/seed.raw"
build/qemu-img create -f qcow2 -F raw -b "$work/seed.raw" "$work/session.qcow2"
build/qemu-img info --backing-chain "$work/session.qcow2"
```

Use `mkdisk6g4k.py` for the upstream retailOS example. It writes MBR
partition offsets in 4 KiB units and adds the FAT marker to the boot
metadata. The older `mkdisk6g.py` uses 512-byte partition units; the two
layouts are not interchangeable.

```sh
build/qemu-system-arm -M iPod-Classic -kernel "$retailos" \
    -serial null -drive "if=ide,format=qcow2,file=$work/session.qcow2"
```

Do not add `-snapshot` if you want writes to survive exit. For subsequent
runs, reuse the same overlay. Do not edit, move, or replace its raw backing
image while the overlay refers to it. Shut down cleanly before backing up
both files, and do not run two writers on one overlay. This is guest ATA
storage, not USB mass storage on the host.

A firmware-free integration check creates disposable empty images, writes and
flushes an ATA sector from the emulated controller, restarts QEMU, compares
the sector, and verifies that neither the neighboring sector nor the raw seed
changed:

```sh
python3 tools/ipod_classic/tests/test_ata_overlay.py \
    build/qemu-system-arm build/qemu-img --bios-dir pc-bios
```

This does not validate mounting, playback, or synchronization in Apple firmware.

## USB connection in the separate Windows Classic7 fork

**The upstream emulator does not expose an attachable USB iPod.** The disk
commands above cannot make an iPod appear in iTunes or Apple Devices. The
Windows workflow below describes the separate desktop fork, not a feature
included in this change. The pinned [Classic7 desktop checkpoint](https://github.com/gamehhh7/qemu-ipod-classic/tree/eb72a619e89d21fe475820ce0d1a0d7c07ac4bab/classic7-desktop)
is source only; a clean Windows build and launch have not been reproduced.

### What you need

- A compatible Windows runtime for the desktop fork: `bin/iPodClassic7.exe`, the matching Classic7 QEMU engine in `engine/`, and the Python host and FC1307 backend dependencies. The checked-in build recipe is an incremental build, not a portable installer. It expects prebuilt QEMU objects and a MinGW toolchain. See the fork's [closeout and migration notes](https://github.com/gamehhh7/qemu-ipod-classic/blob/eb72a619e89d21fe475820ce0d1a0d7c07ac4bab/classic7-desktop/CLOSEOUT-20261116.md). The experimental R140 engine is not the released desktop engine.
- A firmware payload and a compatible raw seed image that you are entitled to use. These are not supplied in the repository. The desktop engine starts the guest with this firmware; the seed supplies the firmware partition and initial disk layout. Keep the seed read-only. Do not publish firmware, music, private device identity, disk images or guest dumps with your changes.
- The Windows USB/IP **client and virtual host-controller driver** that provide `usbip.exe attach` and `detach` for a remote USB device. This checkpoint looks for the client under `Program Files\USBip`; `usbipd-win` alone is not a substitute for the client this code invokes. Use your system's approved driver installation process. Install Apple Devices or iTunes only if you want to test that application's device recognition and media sync.

### Prepare or reuse the fork virtual disk

**Existing installations:** Prefer the existing configuration and overlay. In this checkpoint `userdata/windows-disk.json` selects `userdata/windows-8g.sqlite3`. Shut down through the normal disconnect procedure below before copying either. Preserve the original files and any SQLite sidecars that remain. Use only one backend writer. A missing configured overlay is an error, not an invitation to make a replacement.

**New disks:** The fork records the intended one-time workflow in [`tools/create_windows_disk.py`](https://github.com/gamehhh7/qemu-ipod-classic/blob/eb72a619e89d21fe475820ce0d1a0d7c07ac4bab/classic7-desktop/tools/create_windows_disk.py). It is **not a clean-machine command as shipped**. It depends on an external local FC1307 backend and a particular seed geometry. Its initializer also writes the disk configuration before opening a new overlay, but the shipped runtime store refuses a missing overlay when that configuration is active. A portable initializer must deliberately create a new overlay first, initialize it transactionally, and publish the configuration only after successful validation; the normal launcher should still refuse missing media. Do not bypass that safety check for an existing disk.

The recorded layout preserves the firmware partition and creates an 8 GiB virtual disk with a FAT32 data partition (4,096-byte FAT/USB logical sectors, 32 KiB clusters). ATA uses 512-byte sectors and the firmware performs the mapping. The initializer writes the primary and backup FAT boot sectors, FSInfo and the initial FAT/root metadata. It places the retail firmware's `FAT` marker at byte offset `0x34` in both boot sectors. Unwritten data sectors are sparse zeroes; writable sectors live in the SQLite overlay. The upstream 4 KiB raw-image builder already sets that marker; its raw image or a QCOW2 overlay is a different build path, not a drop-in replacement for this SQLite file.

### Connect and disconnect

1. Start the compatible desktop build with its matching engine, firmware and validated virtual disk. Wait for the guest's normal menus. Use the **USB** button to connect. A desktop build supporting the recorded command-line switch can also connect at startup with `bin\iPodClassic7.exe --connect-usb`.
2. The app checks the guest controller state and imports its own loopback USB/IP export. Let it choose and track the port. Do not install or detach unrelated USB devices, manually expose its listener, or use a diagnostic reset script. Wait for Windows to recognise the virtual iPod before starting a small sync in Apple Devices or iTunes. If title/file or byte persistence matters, verify it after a full eject, application restart and reconnect.
3. Wait until copying and syncing finish. Use **Eject** in the Apple application or Windows' safe-removal UI for the virtual iPod. If ejection is refused, stop the program using the device and retry the safe eject; do not force it during writes. Once ejected, toggle the app's USB button to disconnect its own USB/IP port, then close the desktop window. Closing the window also stops the engine and writer. Do not use **Restore iPod** as part of this workflow.

### What has been verified

The committed desktop records show Windows recognition and file write/read/reconnect persistence, but do not demonstrate a portable clean build or complete Apple-app sync. The fork owner reports that in a later, locally optimized build, both Apple Devices and iTunes identify the virtual iPod and sync songs and videos. The exact local revision was not identified and those observations were not independently reproduced for this guide. Restore still fails and playing video still freezes. Successful video transfer does not imply successful video playback.
