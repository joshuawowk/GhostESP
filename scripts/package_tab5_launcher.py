#!/usr/bin/env python3
"""Package a built GhostESP M5Stack Tab5 image for the M5StackLauncher.

Reads the build's flasher_args.json and merges every flash file at its real
offset into one 0x0-origin image:

    bootloader        @ 0x2000
    partition table   @ 0x8000
    ota_data          @ 0x00d000
    factory app       @ 0x010000
    slave_fw (C6 fw)  @ 0x0d0000-ish (from the partition table)

The result works for BOTH a direct `esptool write-flash 0x0 <img>` and an
M5StackLauncher SD/URL install. It is byte-identical to what `idf.py flash`
would write, so there is no separate "launcher build".

The C6 image sits in a data partition of SPIFFS subtype (0x82) so the Launcher
creates and populates it -- GhostESP finds that partition by name, so the
subtype does not affect firmware behavior. See partitions_m5stack_tab5.csv.

Usage:
    python3 scripts/package_tab5_launcher.py --build-dir build [--out DIR]
    python3 scripts/package_tab5_launcher.py --check <image.bin>   # validate only
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys

BOOTLOADER_OFFSET = 0x2000  # ESP32-P4 second-stage bootloader offset
PART_TABLE_OFFSET = 0x8000
PART_TABLE_MAGIC = 0x50AA   # esp_partition_info_t magic, stored as bytes AA 50


def _load_flasher_args(build_dir):
    with open(os.path.join(build_dir, "flasher_args.json"), "r", encoding="utf-8") as fh:
        return json.load(fh)


def _flash_settings(fa):
    fs = fa.get("flash_settings", {})
    return (
        fs.get("flash_mode", "dio"),
        fs.get("flash_freq", "80m"),
        fs.get("flash_size", "16MB"),
    )


def merge(build_dir, out_dir, name="GhostESP-Tab5-launcher"):
    fa = _load_flasher_args(build_dir)
    chip = fa.get("extra_esptool_args", {}).get("chip", "esp32p4")
    flash_mode, flash_freq, flash_size = _flash_settings(fa)
    flash_files = fa["flash_files"]  # {offset_hex: path_relative_to_build_dir}

    os.makedirs(out_dir, exist_ok=True)
    out_img = os.path.join(out_dir, name + ".bin")

    merge_cmd = [
        sys.executable, "-m", "esptool", "--chip", chip, "merge-bin",
        "-o", out_img,
        "--flash-mode", flash_mode,
        "--flash-freq", flash_freq,
        "--flash-size", flash_size,
    ]
    ordered = sorted(flash_files.items(), key=lambda kv: int(kv[0], 16))
    print("Merging (offset -> file):")
    for off, rel in ordered:
        src = os.path.normpath(os.path.join(build_dir, rel))
        if not os.path.exists(src):
            print(f"ERROR: missing flash file {src} (offset {off})", file=sys.stderr)
            return 1
        print(f"  {off}  {src}")
        merge_cmd += [off, src]

    print("\n$ " + " ".join(merge_cmd))
    result = subprocess.run(merge_cmd)
    if result.returncode != 0:
        print("ERROR: esptool merge-bin failed", file=sys.stderr)
        return result.returncode

    digest = hashlib.sha256()
    with open(out_img, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    sha = digest.hexdigest()
    with open(out_img + ".sha256", "w", encoding="utf-8") as fh:
        fh.write(f"{sha}  {os.path.basename(out_img)}\n")

    size = os.path.getsize(out_img)
    print(f"\nWrote {out_img} ({size} bytes)")
    print(f"sha256: {sha}")
    print("\nInstall:")
    print("  - Direct:   esptool --chip esp32p4 write-flash 0x0 " + os.path.basename(out_img))
    print("  - Launcher: copy to SD, boot the Launcher, SD Card -> select the file")
    return check(out_img)


def check(image_path):
    """Best-effort validation the Launcher's updateFromSD() parser would pass:
    a partition table with the right magic at 0x8000, a bootable app entry, and
    at least one recognized (0x81/0x82/0x83) data partition carrying the C6 fw."""
    with open(image_path, "rb") as fh:
        data = fh.read()
    if len(data) <= PART_TABLE_OFFSET + 0x20:
        print("CHECK FAIL: image too small to hold a partition table", file=sys.stderr)
        return 1
    ok = True
    app_seen = False
    data_seen = []
    for i in range(PART_TABLE_OFFSET, min(len(data), PART_TABLE_OFFSET + 0x1000), 0x20):
        entry = data[i:i + 0x20]
        if len(entry) < 0x20:
            break
        magic = entry[0] | (entry[1] << 8)
        if magic != PART_TABLE_MAGIC:
            break  # end of table
        ptype = entry[2]
        subtype = entry[3]
        offset = int.from_bytes(entry[4:8], "little")
        size = int.from_bytes(entry[8:12], "little")
        label = entry[12:28].split(b"\x00", 1)[0].decode("ascii", "replace")
        if ptype == 0x00:  # app
            app_seen = True
            print(f"  app  {label:<12} off=0x{offset:06x} size=0x{size:06x} sub=0x{subtype:02x}")
        elif ptype == 0x01:  # data
            note = "installed by Launcher" if subtype in (0x81, 0x82, 0x83) else "IGNORED by Launcher"
            data_seen.append((label, subtype, note))
            print(f"  data {label:<12} off=0x{offset:06x} size=0x{size:06x} sub=0x{subtype:02x} ({note})")
    if not app_seen:
        print("CHECK FAIL: no app partition found", file=sys.stderr)
        ok = False
    slave = [d for d in data_seen if d[0] == "slave_fw"]
    if not slave:
        print("CHECK WARN: no slave_fw partition -- the C6 radio image will be missing", file=sys.stderr)
    elif slave[0][1] not in (0x81, 0x82, 0x83):
        print("CHECK FAIL: slave_fw subtype is not 0x81/0x82/0x83 -- the Launcher will "
              "NOT install the C6 image (no WiFi after a Launcher install)", file=sys.stderr)
        ok = False
    print("CHECK OK" if ok else "CHECK FAILED")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default="build", help="ESP-IDF build directory")
    ap.add_argument("--out", default="local_builds", help="output directory")
    ap.add_argument("--name", default="GhostESP-Tab5-launcher",
                    help="output image basename (without .bin)")
    ap.add_argument("--check", metavar="IMAGE", help="validate an existing image and exit")
    args = ap.parse_args()
    if args.check:
        return check(args.check)
    return merge(args.build_dir, args.out, args.name)


if __name__ == "__main__":
    sys.exit(main())
