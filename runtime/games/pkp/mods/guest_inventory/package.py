#!/usr/bin/env python3
"""Deterministically package the standalone guest-inventory Modtopia library."""
import argparse
from pathlib import Path
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED

P = Path(__file__).resolve().parent
PLATFORMS = {
    "macos-arm64": ("mod.dylib", "macos-arm64"),
    "windows-x64": ("mod.dll", "windows-x64"),
}

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--macos-arm64", type=Path)
    p.add_argument("--windows-x64", type=Path)
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    libs = {"macos-arm64": a.macos_arm64, "windows-x64": a.windows_x64}
    if not any(libs.values()):
        p.error("Specify at least one platform binary")
    payload = {"mod.toml": (P / "mod.toml").read_bytes()}
    for platform, path in libs.items():
        if path is None:
            continue
        filename, _ = PLATFORMS[platform]
        if not path.is_file():
            p.error(f"{path} not found")
        payload[f"bin/{platform}/{filename}"] = path.read_bytes()
    a.output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(a.output, "w", ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(payload.items()):
            info = ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, data, compress_type=ZIP_DEFLATED, compresslevel=9)
    print(f"{a.output} ({a.output.stat().st_size:,} bytes; {', '.join(payload)})")

if __name__ == "__main__":
    main()
