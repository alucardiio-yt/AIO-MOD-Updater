# AIO MOD v5.0

AIO MOD es una utilidad para Nintendo Switch derivada de [AIO-Switch-Updater](https://github.com/HamletDuFromage/aio-switch-updater) de HamletDuFromage y sus colaboradores. Esta versión, mantenida por Alucardio, incluye actualizaciones mediante payload, un explorador de archivos integrado y un instalador USB/MTP accesible desde la aplicación.

## Instalación

Descarga [aio-switch-updater-mod.zip](https://github.com/alucardiio-yt/AIO-MOD-Updater/releases/download/v5.0/aio-switch-updater-mod.zip), copia la carpeta `switch` en la raíz de la microSD y ejecuta la aplicación fuera de Applet Mode.

## Código fuente v5.0 y licencias

El [código fuente completo de v5.0](https://github.com/alucardiio-yt/AIO-MOD-Updater/releases/download/v5.0/AIO_MOD_v5.0_codigo.zip) está adjunto a la publicación, con [sumas SHA-256](https://github.com/alucardiio-yt/AIO-MOD-Updater/releases/download/v5.0/SHA256SUMS.txt). Los archivos de código que muestra actualmente la rama `main` y los enlaces automáticos «Source code» de GitHub corresponden a una versión anterior; utiliza el archivo adjunto para v5.0.

El código derivado de AIO-Switch-Updater está disponible bajo [GPL-3.0](LICENSE). El instalador MTP independiente se basa en Sphaira y su subsistema YATI. La procedencia, las adaptaciones y los créditos originales están en [THIRD_PARTY.md](THIRD_PARTY.md) y [THIRD_PARTY_CREDITS.md](THIRD_PARTY_CREDITS.md); el aviso de YATI está en [licenses/YATI-NOTICE.md](licenses/YATI-NOTICE.md) y la [licencia GPL de Sphaira](licenses/SPHAIRA-GPL-3.0.txt) está incluida íntegra. TegraExplorer y las bibliotecas incluidas conservan sus propias licencias. Conserva estos archivos y los avisos originales al redistribuir el código o los binarios.

## Compilación

Se necesita devkitPro para Nintendo Switch. El paquete de código fuente v5.0 incluye el Makefile, el código original de Sphaira 1.0.6 en `vendor/aio-mtp-engine/sphaira-1.0.6-aio-r6` y `tools/build_mtp_engine.py`, que aplica las adaptaciones de AIO MOD antes de compilar el motor MTP.

---

AIO MOD v5.0 for Nintendo Switch is derived from [AIO-Switch-Updater](https://github.com/HamletDuFromage/aio-switch-updater) by HamletDuFromage and contributors. Download [aio-switch-updater-mod.zip](https://github.com/alucardiio-yt/AIO-MOD-Updater/releases/download/v5.0/aio-switch-updater-mod.zip), copy the `switch` folder to the root of the microSD card, and run outside Applet Mode. The [complete v5.0 source archive](https://github.com/alucardiio-yt/AIO-MOD-Updater/releases/download/v5.0/AIO_MOD_v5.0_codigo.zip) and [SHA-256 checksums](https://github.com/alucardiio-yt/AIO-MOD-Updater/releases/download/v5.0/SHA256SUMS.txt) are release assets. The current `main` code tree and GitHub's automatic “Source code” links refer to an earlier version. See [THIRD_PARTY.md](THIRD_PARTY.md), [THIRD_PARTY_CREDITS.md](THIRD_PARTY_CREDITS.md), and [licenses/YATI-NOTICE.md](licenses/YATI-NOTICE.md) for credits and notices.
