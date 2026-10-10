# Third-party software in shifternoumi-setup

The shifternoumi-setup executables bundle the following software. Each keeps its
own license.

| Component | Version | License | Source |
|---|---|---|---|
| xPack OpenOCD | 0.12.0-7 | GPL-2.0-or-later | <https://github.com/xpack-dev-tools/openocd-xpack/releases/tag/v0.12.0-7> |
| libusb (inside OpenOCD) | 1.0.29 | LGPL-2.1-or-later | <https://github.com/libusb/libusb> |
| hidapi (inside OpenOCD) | 0.15.0 | BSD-3-Clause, GPL-3.0 or HIDAPI license | <https://github.com/libusb/hidapi> |
| libftdi1 (inside OpenOCD) | 1.5 | LGPL-2.1 | <https://www.intra2net.com/en/developer/libftdi/> |
| libiconv (inside OpenOCD) | 1.18 | LGPL-2.1-or-later | <https://www.gnu.org/software/libiconv/> |
| Python | 3.x | PSF License | <https://www.python.org> |
| hidapi (Python package) | ≥ 0.14 | BSD-3-Clause, GPL-3.0 or HIDAPI license | <https://github.com/trezor/cython-hidapi> |
| pyserial | ≥ 3.5 | BSD-3-Clause | <https://github.com/pyserial/pyserial> |
| PyInstaller bootloader | 6.x | GPL-2.0 with an exception for bundled applications | <https://github.com/pyinstaller/pyinstaller> |

The full license texts of OpenOCD and the libraries it is built with are
inside the executable, under `openocd/distro-info/licenses/`, as shipped by
the xPack project.

OpenOCD is distributed unmodified. Each GitHub release of ShifterNoumi has the
corresponding source code attached, as `third-party-source-*` archives: OpenOCD at
the exact commit the xPack binary was built from (`e5888bda3`, upstream
openocd-org/openocd), the xPack build scripts (`v0.12.0-7`), and libusb,
hidapi, libftdi1 and libiconv in the versions listed above.
