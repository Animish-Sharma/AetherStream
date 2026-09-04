#!/usr/bin/env python3
"""Verify the exported C ABI against the checked-in versioned manifest."""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import sys
from pathlib import Path


class Config(ctypes.Structure):
    _fields_ = [
        ("target_rate", ctypes.c_float),
        ("absolute_error_bound", ctypes.c_float),
        ("deadzone_factor", ctypes.c_float),
        ("enable_crc", ctypes.c_uint8),
        ("enable_index", ctypes.c_uint8),
    ]


def find_library(build_dir: Path) -> Path:
    patterns = ("aether_c.dll", "libaether_c.so", "libaether_c.dylib")
    matches = [path for pattern in patterns for path in build_dir.rglob(pattern)]
    if not matches:
        versioned = list(build_dir.rglob("libaether_c.so.*"))
        matches.extend(path for path in versioned if path.is_file())
    if not matches:
        raise FileNotFoundError(f"no shared C ABI library found below {build_dir}")
    return sorted(matches, key=lambda path: len(path.parts))[0].resolve()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("build_dir", type=Path)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "abi" / "c_abi_v1.json",
    )
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    library_path = find_library(args.build_dir)
    dll_directory = None
    if sys.platform == "win32":
        dll_directory = os.add_dll_directory(str(library_path.parent))
    library = ctypes.CDLL(str(library_path))

    missing = [name for name in manifest["symbols"] if not hasattr(library, name)]
    if missing:
        raise RuntimeError(f"missing C ABI exports: {', '.join(missing)}")

    library.aether_c_abi_version.restype = ctypes.c_uint32
    library.aether_version_string.restype = ctypes.c_char_p
    library.aether_status_to_string.argtypes = [ctypes.c_int]
    library.aether_status_to_string.restype = ctypes.c_char_p
    library.aether_compress.restype = ctypes.c_int

    assert library.aether_c_abi_version() == manifest["abi_version"]
    assert library.aether_version_string() == b"0.0.1"
    assert ctypes.sizeof(Config) == manifest["config_size"]
    for field, expected in manifest["config_offsets"].items():
        assert getattr(Config, field).offset == expected
    for status in manifest["status_values"]:
        assert library.aether_status_to_string(status)
    assert library.aether_compress(None, 0, None, None, 0, None) == 1
    print(f"verified C ABI v{manifest['abi_version']}: {library_path}")
    if dll_directory is not None:
        dll_directory.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
