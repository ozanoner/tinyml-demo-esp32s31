#!/usr/bin/env python3
"""Read the version an ESP-IDF image reports, straight out of the .bin.

`esp_app_desc_t` sits in DROM at a fixed offset from the start of the application
image — after `esp_image_header_t` and the first `esp_image_segment_header_t`, i.e.
at 0x20 for an app image and further in for a `merged-*.bin`, which begins with the
bootloader. It is the same structure the boot log prints as `app_init: App version:`
and the same one `esp_app_get_description()` returns at runtime, so this is how a
released asset is checked for reporting its own tag without putting it on a board.

The descriptor starts with the magic word 0xABCD5432, which is what locates it; the
version is the 32-byte field 16 bytes after that magic
(`esp_app_format/include/esp_app_desc.h:30`, offsets per
`docs/en/api-reference/system/misc_system_api.rst:291`). Note that the field is
truncated to 31 characters at build time, silently.

Usage:
    app_desc.py dist/app-korvo1.bin                      # report what it says
    app_desc.py dist/*.bin --expect v0.2.0                # fail unless it agrees
    app_desc.py dist/merged-mosaico.bin --expect dispatch-9f3c1ab

Exit 0 when every file was readable and matched, 1 otherwise, with GitHub
`::error::` annotations so a release step fails with a message that names the file.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

MAGIC = 0xABCD5432

# magic_word, secure_version, reserv1[2] precede the strings; then
# version[32], project_name[32], time[16], date[16], idf_ver[32].
FIELDS = struct.Struct("<32s32s16s16s32s")
STRINGS_OFFSET = 16


def fail(message: str, path: Path | None = None) -> None:
    """Print a GitHub annotation and stop."""
    where = f" file={path}" if path is not None else ""
    print(f"::error{where}::{message}")


def text(raw: bytes) -> str:
    """Decode a fixed-width, NUL-padded descriptor string."""
    return raw.split(b"\0")[0].decode("utf-8", errors="replace")


def read_descriptor(path: Path) -> dict[str, str | int] | None:
    """Read the descriptor once; None when the image carries no magic word."""
    blob = path.read_bytes()
    offset = blob.find(struct.pack("<I", MAGIC))
    if offset < 0:
        return None
    version, project, build_time, build_date, idf_ver = FIELDS.unpack_from(
        blob, offset + STRINGS_OFFSET)
    return {
        "version": text(version),
        "project": text(project),
        "built": f"{text(build_date)} {text(build_time)}".strip(),
        "idf": text(idf_ver),
        "offset": offset,
    }


def report(path: Path) -> str:
    """Print one line per image and return the version it reports."""
    found = read_descriptor(path)
    if found is None:
        fail(f"{path.name} carries no esp_app_desc_t magic 0x{MAGIC:08x} — "
             f"not an ESP-IDF application image", path)
        raise SystemExit(1)

    print(f'{path.name}: version "{found["version"]}"  '
          f'project {found["project"]}  idf {found["idf"]}  '
          f'built {found["built"]}  descriptor at 0x{found["offset"]:x}')
    return str(found["version"])


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("images", nargs="+", type=Path,
                        help="application or merged image to read")
    parser.add_argument("--expect", metavar="VERSION",
                        help="fail unless every image reports this version")
    args = parser.parse_args()

    status = 0
    for image in args.images:
        if not image.exists():
            fail(f"{image} does not exist", image)
            status = 1
            continue
        version = report(image)
        if args.expect is not None and version != args.expect:
            fail(f'reports version "{version}" but this release is '
                 f'"{args.expect}"', image)
            status = 1

    if status == 0 and args.expect is not None:
        print(f"version ok: {len(args.images)} image(s) report \"{args.expect}\"")
    return status


if __name__ == "__main__":
    sys.exit(main())
