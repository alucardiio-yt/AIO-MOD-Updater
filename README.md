# AIO MOD

AIO MOD is a Nintendo Switch homebrew utility based on [AIO-Switch-Updater](https://github.com/HamletDuFromage/aio-switch-updater) by HamletDuFromage.
This fork is maintained by Alucardio and includes additional update, download and forwarder features.

## Features

- Update the AIO MOD CFW pack without manually replacing the active CFW files from Horizon.
- Stage and validate a CFW update before rebooting into the bundled updater payload.
- Automatic backup, rollback and resumable cleanup for managed CFW files.
- Download Nintendo Switch firmware packages for installation with Daybreak.
- Download homebrew applications and ports from the configured catalogs.
- Create HOME Menu forwarders for installed `.nro` applications.
- Download cheats and use the utility tools inherited from AIO-Switch-Updater.
- Multi-language interface.

## Installation

Copy the AIO MOD folder to the `switch` directory on the SD card so the application is available at:

```text
/switch/aio-switch-updater-mod/aio-switch-updater-mod.nro
```

Release builds are available from the repository's Releases page.

## Building

A devkitPro Nintendo Switch development environment is required. The main application uses devkitA64/libnx and the Switch portlibs, while the bundled TegraExplorer payload also requires devkitARM. A native C compiler and Python 3 are used by TegraExplorer's build tools on Unix-like systems.

Build from the repository root:

```bash
make clean
make -j$(nproc)
```

The main Makefile also builds the bundled TegraExplorer updater payload, the AIO self-update helper and the HOME Menu forwarder loader.

## Source layout

```text
source/             Main AIO MOD application
include/            Application headers
resources/          UI resources and translations
TegraExplorer/      Source for the CFW updater payload
aiosu-forwarder/    Self-update helper
forwarder-loader/   Loader used by generated HOME Menu forwarders
lib/borealis/       Borealis UI library source
```

## Credits

AIO MOD is derived from AIO-Switch-Updater by HamletDuFromage. The project also includes or derives code from Borealis, TegraExplorer, Sphaira, nx-hbloader and Atmosphère components.

See [THIRD_PARTY.md](THIRD_PARTY.md) and the license files included with each component for details.

## License

The AIO-Switch-Updater-derived application code is distributed under the GNU General Public License v3.0. See [LICENSE](LICENSE).

Bundled third-party components retain their own licenses. In particular, the TegraExplorer directory includes its GPL-2.0 license and Borealis includes its own license notices.

## Disclaimer

Use this software at your own risk. Keep a backup of important SD card and NAND/emuMMC data before performing system or CFW maintenance.
