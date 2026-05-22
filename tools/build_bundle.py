#!/usr/bin/env python3
"""Build CyBeer OTA bundle (.cyb) from firmware and LittleFS images."""

import hashlib
import re
import struct
import sys
from pathlib import Path

MAGIC = b"CYBR"
HEADER_VERSION = 1
HEADER_SIZE = 109  # 4 + 1 + 4 + 4 + 32 + 32 + 32
MAX_FW_SIZE = 0x140000
MAX_FS_SIZE = 0x30000


def get_version_from_cmake(firmware_dir: Path) -> str:
    cmake_file = firmware_dir / "CMakeLists.txt"
    text = cmake_file.read_text()
    m = re.search(r"project\(\s*cybeer\s+VERSION\s+([\d.]+)\s*\)", text)
    if not m:
        sys.exit("ERROR: Could not parse PROJECT_VER from CMakeLists.txt")
    return m.group(1)


def build_bundle(firmware_dir: Path) -> Path:
    version = get_version_from_cmake(firmware_dir)
    fw_path = firmware_dir / "build" / "cybeer.bin"
    fs_path = firmware_dir / "build" / "littlefs.bin"

    if not fw_path.exists():
        sys.exit(f"ERROR: {fw_path} not found. Run 'idf.py build' first.")
    if not fs_path.exists():
        sys.exit(f"ERROR: {fs_path} not found. Run 'idf.py build' first.")

    fw_data = fw_path.read_bytes()
    fs_data = fs_path.read_bytes()

    if len(fw_data) > MAX_FW_SIZE:
        sys.exit(f"ERROR: firmware too large ({len(fw_data)} > {MAX_FW_SIZE})")
    if len(fs_data) > MAX_FS_SIZE:
        sys.exit(f"ERROR: LittleFS image too large ({len(fs_data)} > {MAX_FS_SIZE})")

    fw_sha = hashlib.sha256(fw_data).digest()
    fs_sha = hashlib.sha256(fs_data).digest()

    version_bytes = version.encode("ascii")[:31].ljust(32, b"\x00")

    header = bytearray()
    header += MAGIC
    header += struct.pack("<B", HEADER_VERSION)
    header += struct.pack("<I", len(fw_data))
    header += struct.pack("<I", len(fs_data))
    header += fw_sha
    header += fs_sha
    header += version_bytes

    assert len(header) == HEADER_SIZE

    bundle = bytes(header) + fw_data + fs_data
    out_path = firmware_dir / "build" / f"cybeer-v{version}.cyb"
    out_path.write_bytes(bundle)

    bundle_sha = hashlib.sha256(bundle).hexdigest()
    print(f"Bundle: {out_path}")
    print(f"  Version:  {version}")
    print(f"  Firmware: {len(fw_data)} bytes")
    print(f"  LittleFS: {len(fs_data)} bytes")
    print(f"  Total:    {len(bundle)} bytes")
    print(f"  SHA-256:  {bundle_sha}")
    return out_path


if __name__ == "__main__":
    firmware_dir = Path(__file__).resolve().parent.parent / "firmware"
    if len(sys.argv) > 1:
        firmware_dir = Path(sys.argv[1])
    build_bundle(firmware_dir)
