"""Firmware images: Intel HEX, STM32 CRC, app checks, release packaging.

Packaging (used by CI, also runnable by hand):

    python -m gx100wiz.image package --boot BOOT.bin --app APP.bin \
        --standalone APP_STANDALONE.bin --version v0.1.0 --out dist/

writes dist/gx100-bootloader.bin, gx100-app.bin, gx100-full.hex (first
install: bootloader + app + trailer), gx100-standalone.hex (no bootloader)
and manifest.json.
"""
import argparse
import json
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

from . import layout


def stm32_crc(data: bytes) -> int:
    """CRC of the STM32 CRC unit: CRC-32/MPEG-2 over little-endian words."""
    if len(data) % 4:
        raise ValueError("length must be a multiple of 4")
    crc = 0xFFFFFFFF
    for (word,) in struct.iter_unpack("<I", data):
        crc ^= word
        for _ in range(32):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def pad4(data: bytes) -> bytes:
    return data + b"\xFF" * (-len(data) % 4)


def trailer(app: bytes) -> bytes:
    return struct.pack("<II", len(app), stm32_crc(app))


class ImageError(Exception):
    pass


def check_app(app: bytes) -> None:
    """Same checks as the bootloader's boot_app_valid(), minus the CRC:
    catches a standalone build or a wrong file before anything is erased."""
    if len(app) % 4:
        raise ImageError("image length is not a multiple of 4 (use pad4)")
    if not 8 <= len(app) <= layout.APP_MAX_SIZE:
        raise ImageError(f"image is {len(app)} bytes, the app area holds {layout.APP_MAX_SIZE}")
    sp, pc = struct.unpack_from("<II", app)
    if not layout.RAM_START < sp <= layout.RAM_END:
        raise ImageError(f"initial stack pointer 0x{sp:08X} is not in RAM: not a GX100 app image")
    if not (pc & 1 and layout.APP_BASE <= pc < layout.APP_BASE + len(app)):
        raise ImageError(
            f"reset vector 0x{pc:08X} is outside the app area: this image was not "
            "built for the bootloader (LAYOUT=standalone?)")


# ---- Intel HEX

@dataclass
class Segment:
    address: int
    data: bytes


def write_hex(segments, path) -> None:
    lines = []

    def record(rtype, addr, payload=b""):
        body = bytes([len(payload), (addr >> 8) & 0xFF, addr & 0xFF, rtype]) + payload
        lines.append(":" + body.hex().upper() + f"{(-sum(body)) & 0xFF:02X}")

    for seg in segments:
        upper = None
        for off in range(0, len(seg.data), 16):
            addr = seg.address + off
            if addr >> 16 != upper:
                upper = addr >> 16
                record(0x04, 0, struct.pack(">H", upper))
            record(0x00, addr & 0xFFFF, seg.data[off:off + 16])
    record(0x01, 0)
    Path(path).write_text("\n".join(lines) + "\n", encoding="ascii")


def read_hex(path) -> list:
    """Returns contiguous Segments, sorted by address."""
    chunks = {}
    base = 0
    for n, line in enumerate(Path(path).read_text(encoding="ascii").splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        if not line.startswith(":"):
            raise ImageError(f"{path}:{n}: not an Intel HEX record")
        raw = bytes.fromhex(line[1:])
        if sum(raw) & 0xFF or len(raw) != raw[0] + 5:
            raise ImageError(f"{path}:{n}: bad checksum or length")
        count, addr, rtype, payload = raw[0], (raw[1] << 8) | raw[2], raw[3], raw[4:4 + raw[0]]
        if rtype == 0x00:
            chunks[base + addr] = payload
        elif rtype == 0x01:
            break
        elif rtype == 0x02:
            base = struct.unpack(">H", payload)[0] << 4
        elif rtype == 0x04:
            base = struct.unpack(">H", payload)[0] << 16
        # 0x03 / 0x05: start address, irrelevant for flashing
    segments = []
    for addr in sorted(chunks):
        data = chunks[addr]
        if segments and segments[-1].address + len(segments[-1].data) == addr:
            segments[-1].data.extend(data)
        else:
            segments.append(Segment(addr, bytearray(data)))
    return [Segment(s.address, bytes(s.data)) for s in segments]


# ---- Release packaging

def full_image_segments(boot: bytes, app: bytes) -> list:
    if len(boot) > layout.BOOT_SIZE:
        raise ImageError(f"bootloader is {len(boot)} bytes, the limit is {layout.BOOT_SIZE}")
    return [
        Segment(layout.FLASH_BASE, boot),
        Segment(layout.APP_BASE, app),
        Segment(layout.APP_TRAILER_ADDR, trailer(app)),
    ]


def package(boot_bin: Path, app_bin: Path, standalone_bin: Path, version: str, out: Path) -> dict:
    boot = pad4(boot_bin.read_bytes())
    app = pad4(app_bin.read_bytes())
    check_app(app)
    out.mkdir(parents=True, exist_ok=True)
    (out / "gx100-bootloader.bin").write_bytes(boot)
    (out / "gx100-app.bin").write_bytes(app)
    write_hex(full_image_segments(boot, app), out / "gx100-full.hex")
    files = ["gx100-bootloader.bin", "gx100-app.bin", "gx100-full.hex"]
    if standalone_bin:
        write_hex([Segment(layout.FLASH_BASE, pad4(standalone_bin.read_bytes()))],
                  out / "gx100-standalone.hex")
        files.append("gx100-standalone.hex")
    manifest = {
        "version": version,
        "app": {"file": "gx100-app.bin", "length": len(app), "crc": f"0x{stm32_crc(app):08X}"},
        "bootloader": {"file": "gx100-bootloader.bin", "length": len(boot)},
        "files": files,
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="python -m gx100wiz.image")
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("package", help="build the release files")
    p.add_argument("--boot", type=Path, required=True)
    p.add_argument("--app", type=Path, required=True, help="app built with LAYOUT=bootloader")
    p.add_argument("--standalone", type=Path, help="app built with LAYOUT=standalone")
    p.add_argument("--version", default="dev")
    p.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        manifest = package(args.boot, args.app, args.standalone, args.version, args.out)
    except ImageError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
