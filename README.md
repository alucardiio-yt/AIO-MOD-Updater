# AIO MOD

AIO MOD es una utilidad para Nintendo Switch derivada de [AIO-Switch-Updater](https://github.com/HamletDuFromage/aio-switch-updater) de HamletDuFromage y sus colaboradores. Esta versión, mantenida por Alucardio, incluye actualizaciones mediante payload, un explorador de archivos integrado y un instalador USB/MTP accesible desde la aplicación.

## Instalación

Copia la carpeta `switch` del archivo `aio-switch-updater-mod.zip` en la raíz de la microSD. Ejecuta la aplicación fuera de Applet Mode.

## Código y licencias

El código derivado de AIO-Switch-Updater está disponible bajo [GPL-3.0](LICENSE). El instalador MTP independiente se basa en Sphaira y su subsistema YATI. La procedencia, las adaptaciones y los créditos originales están en [THIRD_PARTY.md](THIRD_PARTY.md) y [THIRD_PARTY_CREDITS.md](THIRD_PARTY_CREDITS.md); el aviso de YATI está en [licenses/YATI-NOTICE.md](licenses/YATI-NOTICE.md). TegraExplorer y las bibliotecas incluidas conservan sus propias licencias. Conserva estos archivos y los avisos originales al redistribuir el código o los binarios.

## Compilación

Se necesita devkitPro para Nintendo Switch. El Makefile construye la aplicación, el actualizador mediante payload, el cargador de forwarders y el motor MTP. El código original de Sphaira 1.0.6 está incluido en `vendor/aio-mtp-engine/sphaira-1.0.6-aio-r6`; [tools/build_mtp_engine.py](tools/build_mtp_engine.py) aplica las adaptaciones de AIO MOD antes de compilarlo.

---

AIO MOD v5.1 for Nintendo Switch is derived from [AIO-Switch-Updater](https://github.com/HamletDuFromage/aio-switch-updater) by HamletDuFromage and contributors. Copy the `switch` folder from `aio-switch-updater-mod.zip` to the root of the microSD card and run outside Applet Mode. The AIO-Switch-Updater-derived code is GPL-3.0; bundled components keep their own licenses. See [THIRD_PARTY.md](THIRD_PARTY.md), [THIRD_PARTY_CREDITS.md](THIRD_PARTY_CREDITS.md) and [licenses/YATI-NOTICE.md](licenses/YATI-NOTICE.md) for credits and notices.
