#!/usr/bin/env python3
"""Check that a GhostESP Tab5 image is one Sentinel's in-launcher Update will pick up.

Sentinel (github.com/joshuawowk/sentinel-bootloader) asks GitHub for the latest release
(newest that is not a draft or pre-release) of the repository named in the image's
SENTINEL_APP_INFO block. It offers that release when its tag is a newer version than the
installed build, and it downloads the release file the block names. So every published
image must:

  - carry the SNAP block at app offset 0x120, naming this repository,
  - be published under exactly the file name the block names,
  - have esp_app_desc_t.version (app offset 0x30) equal to the release tag minus the "v".

Works on the app image (build/Ghost_ESP_IDF.bin) or on the merged 0x0-origin image the
release ships (it finds the app partition through the partition table at 0x8000).

usage:
  sentinel_release_check.py next-version [--floor-file version.txt] < existing-tags
      print the version the next release gets: version.txt when it is ahead of every
      existing X.Y.Z tag, else the newest tag with its patch number bumped
  sentinel_release_check.py asset IMAGE
      print the release file name the image names
  sentinel_release_check.py verify IMAGE --version X.Y.Z --repo OWNER/REPO [--name-matches]
      exit 1 unless the image matches; --name-matches also requires IMAGE's own file
      name to be the one the block names
"""
import argparse
import os
import struct
import sys

ESP_IMAGE_MAGIC = 0xE9
APP_DESC_VERSION = 0x30          # 0x20 (image + segment header) + 16 into esp_app_desc_t
APP_DESC_MAGIC = 0xABCD5432      # esp_app_desc_t.magic_word, at 0x20
SNAP_OFFSET = 0x120
SNAP_MAGIC = 0x50414E53          # "SNAP"
PART_TABLE_OFFSET = 0x8000
PART_MAGIC = 0x50AA


def _cstr(raw):
    return raw.split(b"\0", 1)[0].decode("ascii", "replace")


def app_offset(data):
    """Offset of the app image inside `data`: 0 for an app .bin, else the first app
    partition of a merged image."""
    if data[:1] == bytes([ESP_IMAGE_MAGIC]) and \
            struct.unpack_from("<I", data, 0x20)[0] == APP_DESC_MAGIC:
        return 0
    for i in range(PART_TABLE_OFFSET, min(len(data), PART_TABLE_OFFSET + 0x1000), 0x20):
        magic, ptype, _sub, off, _size = struct.unpack_from("<HBBII", data, i)
        if magic != PART_MAGIC:
            break
        if ptype == 0x00:
            return off
    raise SystemExit("not an ESP app image and no app partition at 0x8000")


def read_info(path):
    with open(path, "rb") as fh:
        data = fh.read()
    base = app_offset(data)
    if len(data) < base + SNAP_OFFSET + 0x100:
        raise SystemExit(f"{path}: image too small")
    if struct.unpack_from("<I", data, base + 0x20)[0] != APP_DESC_MAGIC:
        raise SystemExit(f"{path}: no esp_app_desc_t at app offset 0x20")
    magic, ver, size = struct.unpack_from("<IHH", data, base + SNAP_OFFSET)
    return {
        "app_offset": base,
        "version": _cstr(data[base + APP_DESC_VERSION:base + APP_DESC_VERSION + 32]),
        "snap": magic == SNAP_MAGIC,
        "snap_version": ver,
        "snap_size": size,
        "repo": _cstr(data[base + SNAP_OFFSET + 8:base + SNAP_OFFSET + 128]),
        "asset": _cstr(data[base + SNAP_OFFSET + 128:base + SNAP_OFFSET + 192]),
    }


def _semver(tag):
    t = tag.strip()
    if t[:1] in ("v", "V"):
        t = t[1:]
    parts = t.split(".")
    if len(parts) != 3 or not all(p.isdigit() for p in parts):
        return None
    return tuple(int(p) for p in parts)


def cmd_next_version(args):
    with open(args.floor_file, encoding="utf-8") as fh:
        floor_txt = fh.read().strip()
    floor = _semver(floor_txt)
    if floor is None or floor_txt[:1] in ("v", "V"):
        raise SystemExit(f"{args.floor_file} must hold X.Y.Z, got {floor_txt!r}")
    tags = [v for v in (_semver(line) for line in sys.stdin) if v]
    latest = max(tags) if tags else None
    nxt = floor if latest is None or floor > latest else (latest[0], latest[1], latest[2] + 1)
    print("%d.%d.%d" % nxt)
    return 0


def cmd_asset(args):
    info = read_info(args.image)
    if not info["snap"] or not info["asset"]:
        raise SystemExit(f"{args.image}: no SENTINEL_APP_INFO asset name")
    print(info["asset"])
    return 0


def cmd_verify(args):
    info = read_info(args.image)
    want_repo = "https://github.com/" + args.repo
    problems = []
    if not info["snap"]:
        problems.append("no SNAP block at app offset 0x120 (SENTINEL_APP_INFO missing)")
    else:
        if info["snap_version"] != 1 or info["snap_size"] != 256:
            problems.append(f"SNAP header {info['snap_version']}/{info['snap_size']}, want 1/256")
        if info["repo"].lower() != want_repo.lower():
            problems.append(f"repo is {info['repo']!r}, want {want_repo!r}")
        if not info["asset"].endswith(".bin") or "/" in info["asset"]:
            problems.append(f"asset {info['asset']!r} is not a plain .bin file name")
        if args.name_matches and os.path.basename(args.image) != info["asset"]:
            problems.append(f"file is {os.path.basename(args.image)!r} but the image names "
                            f"{info['asset']!r}; Sentinel would find no update file")
    if info["version"] != args.version:
        problems.append(f"PROJECT_VER is {info['version']!r}, want {args.version!r} "
                        "(version.txt not applied?)")
    print(f"{args.image}: app@0x{info['app_offset']:x} version={info['version']} "
          f"repo={info['repo']} asset={info['asset']}")
    for p in problems:
        print(f"  FAIL: {p}", file=sys.stderr)
    print("SENTINEL CHECK OK" if not problems else "SENTINEL CHECK FAILED")
    return 1 if problems else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    n = sub.add_parser("next-version")
    n.add_argument("--floor-file", default="version.txt")
    a = sub.add_parser("asset")
    a.add_argument("image")
    v = sub.add_parser("verify")
    v.add_argument("image")
    v.add_argument("--version", required=True)
    v.add_argument("--repo", required=True, help="OWNER/REPO")
    v.add_argument("--name-matches", action="store_true")
    args = ap.parse_args()
    return {"next-version": cmd_next_version, "asset": cmd_asset,
            "verify": cmd_verify}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
