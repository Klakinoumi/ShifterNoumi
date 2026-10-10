"""Builds the single-file shifternoumi-setup executable for the current platform.

    python Wizard/build_exe.py --firmware dist --version v0.1.0 --out dist-exe

Bundles the firmware folder (with manifest.json) and xPack OpenOCD, which is
downloaded into Wizard/vendor/openocd on first use. Needs pyinstaller.
"""
import argparse
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
VENDOR = HERE / "vendor"
OPENOCD_VERSION = "0.12.0-7"
OPENOCD_URL = ("https://github.com/xpack-dev-tools/openocd-xpack/releases/download/"
               f"v{OPENOCD_VERSION}/xpack-openocd-{OPENOCD_VERSION}-{{platform}}")
PLATFORMS = {"win32": "win32-x64.zip", "linux": "linux-x64.tar.gz"}


def fetch_openocd() -> Path:
    exe = "openocd.exe" if os.name == "nt" else "openocd"
    target = VENDOR / "openocd"
    if (target / "bin" / exe).is_file():
        return target
    key = "win32" if sys.platform == "win32" else "linux"
    url = OPENOCD_URL.format(platform=PLATFORMS[key])
    print(f"Downloading {url}")
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / PLATFORMS[key]
        urllib.request.urlretrieve(url, archive)
        if archive.suffix == ".zip":
            with zipfile.ZipFile(archive) as z:
                z.extractall(tmp)
        else:
            with tarfile.open(archive) as t:
                t.extractall(tmp, filter="tar")
        top = next(p for p in Path(tmp).iterdir() if p.is_dir() and p.name.startswith("xpack-openocd"))
        VENDOR.mkdir(exist_ok=True)
        shutil.rmtree(target, ignore_errors=True)
        shutil.move(str(top), target)
    return target


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--firmware", type=Path, required=True, help="folder with manifest.json")
    parser.add_argument("--version", default="dev")
    parser.add_argument("--out", type=Path, default=Path("dist-exe"))
    args = parser.parse_args()
    if not (args.firmware / "manifest.json").is_file():
        print(f"{args.firmware}/manifest.json not found (run gx100wiz.image package first)")
        return 1

    openocd = fetch_openocd()
    name = f"shifternoumi-setup-{args.version}-{'windows' if os.name == 'nt' else 'linux'}"
    sep = os.pathsep
    import PyInstaller.__main__
    PyInstaller.__main__.run([
        str(HERE / "shifternoumi-setup.py"),
        "--onefile", "--console", "--noconfirm", "--clean",
        "--name", name,
        "--distpath", str(args.out.resolve()),
        "--workpath", str((HERE / "build").resolve()),
        "--specpath", str((HERE / "build").resolve()),
        "--paths", str(HERE),
        "--hidden-import", "hid",
        "--hidden-import", "serial.tools.list_ports",
        "--add-data", f"{args.firmware.resolve()}{sep}firmware",
        "--add-data", f"{openocd}{sep}openocd",
    ])
    return 0


if __name__ == "__main__":
    sys.exit(main())
