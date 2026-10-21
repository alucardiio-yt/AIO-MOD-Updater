# Créditos y componentes de terceros / Third-party credits

AIO MOD se basa en AIO-Switch-Updater y utiliza componentes de varios proyectos libres. Se conservan sus avisos de autoría y sus licencias originales. Este archivo identifica especialmente el código empleado por el instalador MTP y el creador de forwarders.

## Sphaira y YATI

El instalador MTP independiente de AIO MOD se construye a partir de [Sphaira 1.0.6](https://github.com/NaGaa95/sphaira/tree/1.0.6), revisión `3f8303db00f33bfffa83ce0a1b750a1de14656e2`, desarrollado por **ITotalJustice / TotalJustice** y sus colaboradores. La fuente original se conserva en `vendor/aio-mtp-engine/sphaira-1.0.6-aio-r6`. **YATI** es el subsistema de instalación incluido en el árbol de Sphaira (`sphaira/source/yati` y `sphaira/include/yati`); en esa versión no aparece como proyecto con licencia independiente. Su código queda cubierto por la licencia **GPL-3.0 de Sphaira**, cuya copia íntegra está en [licenses/SPHAIRA-GPL-3.0.txt](licenses/SPHAIRA-GPL-3.0.txt). Véase también [licenses/YATI-NOTICE.md](licenses/YATI-NOTICE.md).

AIO MOD modifica la presentación, traducciones, registro de actividad, arranque y retorno del instalador, exposición de la microSD por MTP y tratamiento de transferencias consecutivas. El script [tools/build_mtp_engine.py](tools/build_mtp_engine.py) identifica la versión upstream y aplica esas adaptaciones. El motor se ejecuta como NRO independiente; este proyecto no afirma ser la versión original ni contar con el aval de los autores de Sphaira.

El creador de forwarders de AIO MOD adapta el flujo NCA/RomFS de `owo.cpp` de Sphaira. El comentario de atribución se conserva en [source/forwarder_installer.cpp](source/forwarder_installer.cpp).

### Créditos conservados del README de Sphaira 1.0.6

- [borealis](https://github.com/natinusala/borealis)
- [stb](https://github.com/nothings/stb)
- [yyjson](https://github.com/ibireme/yyjson)
- [nx-hbmenu](https://github.com/switchbrew/nx-hbmenu)
- [nx-hbloader](https://github.com/switchbrew/nx-hbloader)
- [deko3d-nanovg](https://github.com/Adubbz/nanovg-deko3d)
- [libpulsar](https://github.com/p-sam/switch-libpulsar)
- [minIni](https://github.com/compuphase/minIni)
- [GBATemp](https://gbatemp.net/threads/sphaira-hbmenu-replacement.664523/)
- [hb-appstore](https://github.com/fortheusers/hb-appstore)
- [haze](https://github.com/Atmosphere-NX/Atmosphere/tree/master/troposphere/haze)
- [nxdumptool](https://github.com/DarkMatterCore/nxdumptool), por el código de extracción de gamecard y verificación RSA
- [Liam0](https://github.com/ThatNerdyPikachu/switch-010editor-templates), por las estructuras de tickets y certificados
- [libusbhsfs](https://github.com/DarkMatterCore/libusbhsfs)
- [libnxtc](https://github.com/DarkMatterCore/libnxtc)
- [oss-nvjpg](https://github.com/averne/oss-nvjpg)
- [nsz](https://github.com/nicoboss/nsz)
- [themezer](https://themezer.net/)
- [nx-versions](https://github.com/16BitWonder/nx-versions)
- [NX-Update-Checker](https://github.com/16BitWonder/NX-Update-Checker)
- Todas las personas que han contribuido a Sphaira.

Los avisos y licencias propios de los componentes incluidos en el árbol del código siguen en sus archivos originales. La lista de créditos no sustituye sus condiciones de licencia.

---

AIO MOD builds its separate MTP installer from Sphaira 1.0.6. YATI is part of that Sphaira source tree and is covered by Sphaira's GPL-3.0; no separate YATI license was found in the pinned version. AIO MOD's changes and the upstream credits are identified above. Original third-party copyright and license notices remain in their respective files.
