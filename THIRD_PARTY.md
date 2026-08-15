# Third-party software and credits

AIO MOD is built on existing open-source Nintendo Switch projects. This file summarizes the main upstream components used by this source tree. License texts and notices shipped with the corresponding components must be kept with redistributed source.

## AIO-Switch-Updater

- Upstream: https://github.com/HamletDuFromage/aio-switch-updater
- Author/maintainer of the original project: HamletDuFromage and contributors
- License: GNU GPL v3.0
- Use in AIO MOD: base application and utility code

AIO MOD contains substantial modifications and additional functionality but remains derived from AIO-Switch-Updater.

## Borealis

- Included under: `lib/borealis/`
- Upstream family: Borealis Nintendo Switch UI library
- License notices: `lib/borealis/LICENSE` and `lib/borealis/COPYING`

## TegraExplorer

- Included under: `TegraExplorer/`
- Upstream used by AIO-Switch-Updater: https://github.com/HamletDuFromage/TegraExplorer
- License: GNU GPL v2.0
- Use in AIO MOD: the bundled CFW updater payload is implemented as a modified TegraExplorer build

The modified updater source is included in `TegraExplorer/source/tegraexplorer/aio_cfw_updater.c` and related files.

## Sphaira

- Upstream: https://github.com/ITotalJustice/sphaira
- License: GNU GPL v3.0
- Use in AIO MOD: portions of the forwarder creation flow are derived from Sphaira's forwarder implementation. The relevant attribution is also retained in `source/forwarder_installer.cpp` and `forwarder-loader/Makefile`.

## nx-hbloader

- License notice: `forwarder-loader/nx-hbloader.LICENSE.md`
- Use in AIO MOD: loader-related code used by the generated forwarder runtime

## Atmosphère

Some service definitions and interfaces used by the project originate from Atmosphère. Source files that carry Atmosphère copyright/license headers retain those notices.

## Original AIO-Switch-Updater contributors

AIO MOD retains the original project's license and attribution. For the full upstream contributor acknowledgements, see the AIO-Switch-Updater repository linked above.
