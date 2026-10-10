"""Client for the USB bootloader protocol (gx100_boot.h).

The transport is pluggable: HidTransport talks to the real device,
PipeTransport to Bootloader/test/boot_proto_host (host tests).
"""
import struct
import subprocess
from dataclasses import dataclass

from . import layout
from .image import check_app, stm32_crc


class BootloaderError(Exception):
    pass


class HidTransport:
    def __init__(self, path: bytes, timeout_ms: int = 2000):
        import hid  # hidapi; imported here so the tests do not need it
        self._dev = hid.device()
        self._dev.open_path(path)
        self._timeout_ms = timeout_ms

    def exchange(self, request: bytes) -> bytes:
        # Leading 0 = report ID (the device uses none). hidapi reports a
        # device unplugged mid-update either as a failed write (-1) or as
        # an OSError, on the write or on the read, depending on timing.
        try:
            if self._dev.write(b"\x00" + request) < 0:
                raise BootloaderError("USB write failed (device unplugged?)")
            response = bytes(self._dev.read(layout.REPORT_SIZE, self._timeout_ms))
        except OSError as e:
            raise BootloaderError(f"USB error ({e}): device unplugged?") from e
        if len(response) != layout.REPORT_SIZE:
            raise BootloaderError("no answer from the bootloader")
        return response

    def close(self):
        self._dev.close()


class PipeTransport:
    """Runs a boot_proto_host binary and talks to it over stdin/stdout."""

    def __init__(self, argv):
        self._proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def exchange(self, request: bytes) -> bytes:
        # A simulated device that has "reset" (BOOT) may already have
        # exited: like a USB write to an unplugged device.
        try:
            self._proc.stdin.write(request)
            self._proc.stdin.flush()
        except (BrokenPipeError, OSError) as e:
            raise BootloaderError("simulated device stopped") from e
        response = self._proc.stdout.read(layout.REPORT_SIZE)
        if len(response) != layout.REPORT_SIZE:
            raise BootloaderError("simulated device stopped")
        return response

    def close(self):
        self._proc.stdin.close()
        self._proc.wait(timeout=5)
        self._proc.stdout.close()


@dataclass
class BootInfo:
    proto: int
    version: str
    app_valid: bool
    app_base: int
    app_max: int
    page_size: int
    app_length: int
    app_crc: int
    uid: str
    flash_kb: int

    def installed_matches(self, app: bytes) -> bool:
        return self.app_valid and self.app_length == len(app) and self.app_crc == stm32_crc(app)


class BootloaderClient:
    def __init__(self, transport):
        self._t = transport
        self._seq = 0

    def _request(self, cmd: int, args: bytes = b"") -> bytes:
        self._seq = (self._seq + 1) & 0xFF
        req = bytes([cmd, self._seq, 0, 0]) + args
        req += b"\x00" * (layout.REPORT_SIZE - len(req))
        resp = self._t.exchange(req)
        if resp[0] != cmd or resp[1] != self._seq:
            raise BootloaderError(f"out-of-sequence answer (cmd {resp[0]:#x}, seq {resp[1]})")
        status = resp[2]
        if status != layout.STATUS_OK:
            text = layout.STATUS_TEXT.get(status, f"status {status}")
            raise BootloaderError(f"bootloader refused command {cmd:#x}: {text}")
        return resp

    def info(self) -> BootInfo:
        r = self._request(layout.CMD_INFO)
        if r[layout.INFO_MAGIC:layout.INFO_MAGIC + 4] != b"GXBL":
            raise BootloaderError("not a GX100 bootloader")
        u32 = lambda off: struct.unpack_from("<I", r, off)[0]
        return BootInfo(
            proto=r[layout.INFO_PROTO],
            version=f"{r[layout.INFO_VER_MAJOR]}.{r[layout.INFO_VER_MINOR]}",
            app_valid=bool(r[layout.INFO_APP_VALID]),
            app_base=u32(layout.INFO_APP_BASE),
            app_max=u32(layout.INFO_APP_MAX),
            page_size=u32(layout.INFO_PAGE_SIZE),
            app_length=u32(layout.INFO_APP_LENGTH),
            app_crc=u32(layout.INFO_APP_CRC),
            uid=r[layout.INFO_UID:layout.INFO_UID + 12].hex().upper(),
            flash_kb=struct.unpack_from("<H", r, layout.INFO_FLASH_KB)[0],
        )

    def update(self, app: bytes, progress=None) -> None:
        """Erase, write, commit. On any failure the device keeps an invalid
        app and stays in the bootloader: running update() again recovers."""
        check_app(app)
        info = self.info()
        if info.proto != layout.PROTO_VERSION:
            raise BootloaderError(f"bootloader speaks protocol {info.proto}, this wizard {layout.PROTO_VERSION}")
        if info.app_base != layout.APP_BASE or len(app) > info.app_max:
            raise BootloaderError("bootloader flash layout differs from this wizard's")

        self._request(layout.CMD_ERASE)
        step = layout.WRITE_MAX_LEN
        for off in range(0, len(app), step):
            chunk = app[off:off + step]
            args = struct.pack("<IB3x", off, len(chunk)) + chunk
            self._request(layout.CMD_WRITE, args)
            if progress:
                progress(off + len(chunk), len(app))
        self._request(layout.CMD_COMMIT, struct.pack("<II", len(app), stm32_crc(app)))

    def boot(self) -> None:
        self._request(layout.CMD_BOOT)
