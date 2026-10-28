#!/usr/bin/env python3
"""Build the pinned Sphaira MTP/YATI engine used by AIO MOD.

This script intentionally keeps the upstream engine in a separate NRO/process.
It clones the exact Sphaira 1.0.6 tag, applies a small reproducible AIO-MTP
patch, builds it with devkitPro, and copies the result into AIO MOD's RomFS.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from mtp_aio_ui.patch_haze_install import patch_file as patch_haze_install

UPSTREAM_REPO = "https://github.com/NaGaa95/sphaira.git"
UPSTREAM_TAG = "1.0.6"
AIO_PATCH_REV = "aio-mtp-engine-r8"
ENGINE_STAMP = f"sphaira-{UPSTREAM_TAG}-{AIO_PATCH_REV}"

LEGACY_HINT = (
    "Install the Sphaira build dependencies in your devkitPro MSYS2 shell:\n"
    "  pacman -S --needed switch-dev deko3d switch-cmake switch-curl "
    "switch-glm switch-zlib switch-mbedtls git cmake make zip\n"
)


def run(cmd: list[str], cwd: Path | None = None) -> None:
    print("[AIO MTP]", " ".join(cmd))
    subprocess.run(cmd, cwd=str(cwd) if cwd else None, check=True)


def replace_once(path: Path, old: str, new: str, label: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"Patch '{label}' expected 1 match in {path}, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def replace_between(path: Path, start_marker: str, end_marker: str, replacement: str, label: str) -> None:
    text = path.read_text(encoding="utf-8")
    start = text.find(start_marker)
    if start < 0:
        raise RuntimeError(f"Patch '{label}' start marker was not found in {path}")
    end = text.find(end_marker, start)
    if end < 0:
        raise RuntimeError(f"Patch '{label}' end marker was not found in {path}")
    path.write_text(text[:start] + replacement + text[end:], encoding="utf-8")


def valid_nro(path: Path) -> bool:
    try:
        if path.stat().st_size < 0x1000:
            return False
        with path.open("rb") as f:
            f.seek(0x10)
            return f.read(4) == b"NRO0"
    except OSError:
        return False


def ensure_tools() -> None:
    missing = [tool for tool in ("git", "cmake") if shutil.which(tool) is None]
    if missing:
        raise RuntimeError(f"Missing required tool(s): {', '.join(missing)}\n{LEGACY_HINT}")

    devkitpro = os.environ.get("DEVKITPRO")
    if not devkitpro:
        raise RuntimeError("DEVKITPRO is not defined. Open the devkitPro/MSYS2 shell before compiling.")
    switch_cmake = Path(devkitpro) / "cmake" / "Switch.cmake"
    if not switch_cmake.exists():
        raise RuntimeError(f"switch-cmake was not found at {switch_cmake}.\n{LEGACY_HINT}")


def get_source(project_root: Path, force: bool) -> Path:
    vendor_root = project_root / "vendor" / "aio-mtp-engine"
    # Keep this revision beside earlier vendored snapshots. Some distributed
    # source ZIPs intentionally contain a reduced snapshot with build output;
    # a fresh checkout guarantees that UI assets and CMake patches are present.
    source = vendor_root / f"sphaira-{UPSTREAM_TAG}-aio-r6"
    vendor_root.mkdir(parents=True, exist_ok=True)

    if not source.exists():
        run([
            "git", "clone", "--depth", "1", "--branch", UPSTREAM_TAG,
            UPSTREAM_REPO, str(source),
        ])
    elif force and (source / ".git").exists():
        run(["git", "fetch", "--depth", "1", "origin", f"refs/tags/{UPSTREAM_TAG}:refs/tags/{UPSTREAM_TAG}"], cwd=source)

    # Git checkouts are reset so a rebuild always starts from the selected
    # upstream tag before the reproducible AIO overlay is applied.
    if (source / ".git").exists():
        run(["git", "reset", "--hard", UPSTREAM_TAG], cwd=source)
        run(["git", "clean", "-fdx"], cwd=source)
    return source


def patch_source(source: Path, project_root: Path) -> None:
    main_cpp = source / "sphaira" / "source" / "main.cpp"
    app_hpp = source / "sphaira" / "include" / "app.hpp"
    haze_cpp = source / "sphaira" / "source" / "haze_helper.cpp"
    mtp_cpp = source / "sphaira" / "source" / "ui" / "menus" / "mtp_menu.cpp"
    cmake = source / "sphaira" / "CMakeLists.txt"
    overlay = Path(__file__).resolve().parent / "mtp_aio_ui"

    main_text = main_cpp.read_text(encoding="utf-8")
    already_isolated = "AIO MOD launches this helper intentionally as an installer" in main_text

    if not already_isolated:
        replace_once(
            main_cpp,
            '#include <memory>\n',
            '#include <memory>\n#include <string>\n',
            "string include",
        )

        replace_once(
            main_cpp,
            '#include "ui/menus/main_menu.hpp"\n',
            '#include "ui/menus/mtp_menu.hpp"\n#include "ui/menus/menu_base.hpp"\n',
            "direct MTP menu include",
        )

        replace_once(
            main_cpp,
            '''    auto app = std::make_unique<sphaira::App>(argv[0]);\n    app->Push<sphaira::ui::menu::main::MainMenu>();\n    app->Loop();\n    return 0;\n''',
            '''    auto app = std::make_unique<sphaira::App>(argv[0]);\n\n    // AIO MOD launches this helper intentionally as an installer. Keep the\n    // engine isolated from the user's normal Sphaira configuration, allow\n    // installation on either sysMMC or emuMMC, and target SD by default.\n    sphaira::App::SetMtpEnable(false);\n    sphaira::App::SetInstallSysmmcEnable(true);\n    sphaira::App::SetInstallEmummcEnable(true);\n    sphaira::App::SetInstallSdEnable(true);\n\n    app->Push<sphaira::ui::menu::mtp::Menu>(sphaira::ui::menu::MenuFlag_None);\n    app->Loop();\n\n    // Returning with B closes the helper and chainloads AIO MOD again.\n    const char* aio_path = "sdmc:/switch/aio-switch-updater-mod/aio-switch-updater-mod.nro";\n    const std::string aio_argv = std::string("\\\"") + aio_path + "\\\"";\n    envSetNextLoad(aio_path, aio_argv.c_str());\n    return 0;\n''',
            "direct MTP menu boot and return",
        )

    if 'CONFIG_PATH = "/config/sphaira/config.ini"' in app_hpp.read_text(encoding="utf-8"):
        replace_once(
            app_hpp,
            '    static constexpr inline auto CONFIG_PATH = "/config/sphaira/config.ini";\n'
            '    static constexpr inline auto PLAYLOG_PATH = "/config/sphaira/playlog.ini";\n',
            '    static constexpr inline auto CONFIG_PATH = "/config/aio-switch-updater-mod/sphaira_mtp_engine.ini";\n'
            '    static constexpr inline auto PLAYLOG_PATH = "/config/aio-switch-updater-mod/sphaira_mtp_playlog.ini";\n',
            "isolated AIO MTP configuration",
        )

    if '#include "ui/activity_log.hpp"' not in haze_cpp.read_text(encoding="utf-8"):
        replace_once(
            haze_cpp,
            '#include "i18n.hpp"\n',
            '#include "i18n.hpp"\n#include "ui/activity_log.hpp"\n',
            "microSD copy progress include",
        )

    if "struct FsTrackedProxy final" not in haze_cpp.read_text(encoding="utf-8"):
        tracker = (overlay / "tracked_fs_proxy.inc").read_text(encoding="utf-8")
        replace_once(
            haze_cpp,
            "struct FsProxyVfs : FsProxyBase {\n",
            tracker + "\nstruct FsProxyVfs : FsProxyBase {\n",
            "microSD copy progress proxy",
        )

    mount_new = '''    // AIO MOD helper: keep normal microSD file access separate from\n    // the virtual installation target. Files copied to the microSD mount use\n    // the regular filesystem proxy and never enter the YATI installer path.\n    g_fs_entries.clear();\n    g_install_fs.reset();\n    g_fs_entries.emplace_back(std::make_shared<FsTrackedProxy>(\n        std::make_shared<FsProxy>(std::make_unique<fs::FsNativeSd>(), "", "microSD card")));\n    g_install_fs = std::make_shared<FsInstallProxy>("install", "SD Card Install");\n    g_fs_entries.emplace_back(g_install_fs);\n\n'''
    if "// add default mount of the sd card." in haze_cpp.read_text(encoding="utf-8"):
        replace_between(
            haze_cpp,
            "    // add default mount of the sd card.\n",
            "    g_should_exit = false;\n",
            mount_new,
            "AIO MTP mounts",
        )

    patch_haze_install(haze_cpp)

    if 'NAME ${CMAKE_PROJECT_NAME}' in cmake.read_text(encoding="utf-8"):
        replace_once(
            cmake,
            '    NAME ${CMAKE_PROJECT_NAME}\n    AUTHOR TotalJustice\n',
            '    NAME "AIO MOD"\n    AUTHOR "Alucardio"\n',
            "helper NACP identity",
        )
        replace_once(
            cmake,
            '    ICON ${CMAKE_SOURCE_DIR}/assets/icon.jpg\n',
            '    ICON ${CMAKE_SOURCE_DIR}/assets/aio_mod_icon.png\n',
            "AIO helper icon",
        )
    elif 'AUTHOR "Alucardio / Sphaira"' in cmake.read_text(encoding="utf-8"):
        replace_once(
            cmake,
            '    AUTHOR "Alucardio / Sphaira"\n',
            '    AUTHOR "Alucardio"\n',
            "AIO helper author",
        )

    # Apply the AIO MOD presentation and sequential MTP stream fix as an
    # overlay. YATI's installer and libhaze protocol implementation stay intact.
    shutil.copy2(overlay / "menu_base.cpp", source / "sphaira/source/ui/menus/menu_base.cpp")
    shutil.copy2(overlay / "mtp_menu.cpp", mtp_cpp)
    shutil.copy2(overlay / "mtp_menu.hpp", source / "sphaira/include/ui/menus/mtp_menu.hpp")
    shutil.copy2(overlay / "install_stream_menu_base.cpp", source / "sphaira/source/ui/menus/install_stream_menu_base.cpp")
    shutil.copy2(overlay / "install_stream_menu_base.hpp", source / "sphaira/include/ui/menus/install_stream_menu_base.hpp")
    shutil.copy2(overlay / "progress_box.cpp", source / "sphaira/source/ui/progress_box.cpp")
    shutil.copy2(overlay / "progress_box.hpp", source / "sphaira/include/ui/progress_box.hpp")
    shutil.copy2(overlay / "activity_log.cpp", source / "sphaira/source/ui/activity_log.cpp")
    shutil.copy2(overlay / "activity_log.hpp", source / "sphaira/include/ui/activity_log.hpp")

    if "source/ui/activity_log.cpp" not in cmake.read_text(encoding="utf-8"):
        replace_once(
            cmake,
            "    source/ui/nvg_util.cpp\n",
            "    source/ui/nvg_util.cpp\n    source/ui/activity_log.cpp\n",
            "activity log source",
        )

    themes = source / "assets/romfs/themes"
    themes.mkdir(parents=True, exist_ok=True)
    shutil.copy2(overlay / "aio_mod_theme.ini", themes / "aio_mod_theme.ini")
    icon_source = project_root / "resources/gui_icon.png"
    if not icon_source.exists():
        icon_source = Path(__file__).resolve().parents[1] / "resources/gui_icon.png"
    shutil.copy2(icon_source, source / "assets/romfs/aio_mod_icon.png")
    shutil.copy2(icon_source, source / "assets/aio_mod_icon.png")

    # Use AIO MOD's selected language when present. With no selection, the
    # helper keeps Sphaira's automatic system-language behaviour.
    if "aioLanguageIndex" not in main_cpp.read_text(encoding="utf-8"):
        replace_once(
            main_cpp,
            '#include "utils/core.hpp"\n',
            '#include "utils/core.hpp"\n#include "i18n.hpp"\n#include <cstdio>\n#include <cstring>\n',
            "AIO language includes",
        )
        replace_once(
            main_cpp,
            "int main(int argc, char** argv) {\n",
            '''namespace {\nlong aioLanguageIndex() {\n    FILE* file = std::fopen("sdmc:/config/aio-switch-updater-mod/language.json", "rb");\n    if (!file) return -1;\n    char data[256]{};\n    const auto read = std::fread(data, 1, sizeof(data) - 1, file);\n    std::fclose(file);\n    data[read] = '\\0';\n    if (std::strstr(data, "\\\"es\\\"")) return 6;\n    if (std::strstr(data, "\\\"fr\\\"")) return 3;\n    if (std::strstr(data, "\\\"en-US\\\"") || std::strstr(data, "\\\"en\\\"")) return 1;\n    return -1;\n}\n}\n\nint main(int argc, char** argv) {\n''',
            "AIO language reader",
        )
        replace_once(
            main_cpp,
            "    auto app = std::make_unique<sphaira::App>(argv[0]);\n",
            '''    auto app = std::make_unique<sphaira::App>(argv[0]);\n\n    if (const auto language = aioLanguageIndex(); language >= 0) {\n        sphaira::i18n::exit();\n        sphaira::i18n::init(language);\n    }\n    for (size_t i = 0; i < sphaira::App::GetThemeMetaList().size(); ++i) {\n        if (sphaira::App::GetThemeMetaList()[i].name == "AIO MOD") {\n            sphaira::App::SetTheme(static_cast<s64>(i));\n            break;\n        }\n    }\n    sphaira::App::SetThemeMusicEnable(false);\n''',
            "AIO language and theme activation",
        )

    translations = json.loads((overlay / "translations.json").read_text(encoding="utf-8"))
    log_translations = json.loads((overlay / "translations_log.json").read_text(encoding="utf-8"))
    for language, values in log_translations.items():
        translations.setdefault(language, {}).update(values)
    fallback = translations["en"]
    for language_file in (source / "assets/romfs/i18n").glob("*.json"):
        language_data = json.loads(language_file.read_text(encoding="utf-8"))
        language_data.update(fallback)
        language_data.update(translations.get(language_file.stem, {}))
        language_file.write_text(
            json.dumps(language_data, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )


def configure_and_build(source: Path) -> Path:
    # Use a revision-specific build folder so source ZIPs containing an older
    # CMake cache can be rebuilt safely from any location.
    build = source / "build" / "AioMtpEngineR7"
    args = [
        "cmake", "-S", str(source), "-B", str(build),
        "-DCMAKE_BUILD_TYPE=MinSizeRel",
        "-DCMAKE_DEPENDS_USE_COMPILER=FALSE",
        "-DLTO=ON",
        "-DENABLE_NVJPG=OFF",
        "-DENABLE_NSZ=ON",
        "-DENABLE_LIBUSBHSFS=OFF",
        "-DENABLE_LIBUSBDVD=OFF",
        "-DENABLE_FTPSRV=OFF",
        "-DENABLE_LIBHAZE=ON",
        "-DENABLE_AUDIO_MP3=OFF",
        "-DENABLE_AUDIO_OGG=OFF",
        "-DENABLE_AUDIO_WAV=OFF",
        "-DENABLE_AUDIO_FLAC=OFF",
        "-DENABLE_DEVOPTAB_HTTP=OFF",
        "-DENABLE_DEVOPTAB_NFS=OFF",
        "-DENABLE_DEVOPTAB_SMB2=OFF",
        "-DENABLE_DEVOPTAB_FTP=OFF",
        "-DENABLE_DEVOPTAB_SFTP=OFF",
        "-DENABLE_DEVOPTAB_WEBDAV=OFF",
        f"-Dsphaira_VERSION_OVERRIDE=AIO-MTP-{UPSTREAM_TAG}",
    ]
    try:
        run(args, cwd=source)
        run(["cmake", "--build", str(build), "--parallel", str(max(1, min(os.cpu_count() or 4, 16)))], cwd=source)
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(
            f"Sphaira MTP engine build failed (command exit {exc.returncode}).\n{LEGACY_HINT}"
        ) from exc

    nro = build / "sphaira.nro"
    if not valid_nro(nro):
        raise RuntimeError(f"Build completed but a valid NRO was not found at {nro}")
    return nro


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    project_root = args.project_root.resolve()
    output = args.output.resolve()
    stamp = project_root / "vendor" / "aio-mtp-engine" / ".engine-stamp"

    if not args.force and valid_nro(output) and stamp.exists() and stamp.read_text(encoding="utf-8").strip() == ENGINE_STAMP:
        print(f"[AIO MTP] Using cached {ENGINE_STAMP}: {output}")
        return 0

    ensure_tools()
    source = get_source(project_root, args.force)
    patch_source(source, project_root)
    built = configure_and_build(source)

    output.parent.mkdir(parents=True, exist_ok=True)
    temp = output.with_suffix(output.suffix + ".tmp")
    shutil.copy2(built, temp)
    if not valid_nro(temp):
        temp.unlink(missing_ok=True)
        raise RuntimeError("Generated MTP engine failed NRO validation")
    temp.replace(output)

    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.write_text(ENGINE_STAMP + "\n", encoding="utf-8")
    print(f"[AIO MTP] Engine ready: {output} ({output.stat().st_size:,} bytes)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[AIO MTP] ERROR: {exc}\n", file=sys.stderr)
        raise SystemExit(1)
