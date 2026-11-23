"""The 32-bit Android port's build (configure.py --console-port: chupathingyce's
tools/console_build.py), ``ninja android32``.

For the Android devices whose system is 32-bit only (armeabi-v7a), which the
upstream app's native 64-bit build (arm64-v8a) cannot run on: the cheap
Android TV boxes (an Amlogic S905W2 has 64-bit cores and a 32-bit Android),
many Fire TV sticks and old tablets. The game and the platform layer are the
32-bit Linux build's (as the Steam Link's, port/steamlink/build.py), compiled
with the NDK's clang for bionic and linked into libmain.so, the library SDL3's
SDLActivity loads and whose SDL_main (src/android32_main.c) it calls. The GPU
is OpenGL ES 3 (the renderer's ES path, HALO_GL_ES), called directly.

    cd chupathingyce
    python3 configure.py --release --console-port ../port/android32
    ninja android32

--android32-ndk is the NDK (ANDROID_NDK_HOME, or the newest under ANDROID_HOME),
--android32-sdl SDL3 built for armeabi-v7a (build/android32/sdl3-build in
this repository: cmake with the NDK's toolchain file, SDL3 source beside it).
"""

import os
from pathlib import Path
from types import SimpleNamespace
from typing import Any, List, Optional

from tools.embed_assets import hud_assets_build, hud_configure_inputs, ui_fonts_build
from tools.console_build import console_defines
from tools.linux_build import Linux32Units, _load_port_config, compile_launcher, linux32_objects, lto_flags

ANDROID_API = 28
TRIPLE = f"armv7a-linux-androideabi{ANDROID_API}"
# Android's armeabi-v7a is the soft-float calling convention with VFP and NEON
# instructions (softfp). The game's char is MSVC's, signed (ARM's is unsigned);
# no VFP registers for what is not floating point (-mno-implicit-float: a
# structure of floats copied with vldr faults on the 2-byte aligned ones in a
# map's animation data)
# (-fPIC last: the 32-bit Linux flags have -fno-pic before the instruction set, and a shared library needs
# every unit, the third-party ones too, position independent)
INSTRUCTION_SET = ["-march=armv7-a", "-mfpu=neon-vfpv4", "-mfloat-abi=softfp", "-fPIC"]
ABI_FLAGS = ["-fsigned-char", "-mno-implicit-float", "-fPIC", "-DHALO_ANDROID32", "-DHALO_GL_ES=1",
              # the touch controls and the menus' touch pointer (port/linux/src/touch_input.c)
              "-DHALO_TOUCH_SCREEN"]
CAPABILITIES = [
    # config.toml in the data folder (HALO_DATA_ROOT, src/android32_main.c)
    "-DHALO_CONSOLE_CONFIG_IN_DATA_ROOT",
    # the start-up screens are the port's (src/android32_setup.c): no launcher window
    "-DHALO_CONSOLE_SETUP_SCREENS",
    # the consoles' limits that keep memory free (Android takes most of a
    # 1.8 GB box): the Xbox's texture cache, the high-res HUD decoded as it
    # is drawn
    "-DHALO_CONSOLE_XBOX_RENDER_SIZES",
    "-DHALO_CONSOLE_HUD_ON_DEMAND",
    # no screenshots or clips of the desktop's (mapped buffers and fences)
    "-DHALO_CONSOLE_NO_CAPTURE",
    "-DHALO_CONSOLE_NO_GUIDE",
    "-DHALO_CONSOLE_NO_MEDAL_ICONS",
    "-DHALO_CONSOLE_NO_TITLE_CHOICE",
]
# (the entry point and the GL helpers talk to bionic and ES only, as posix_*.c do)
NATIVE_SOURCES = ["src/android32_main.c", "src/android32_gl.c", "src/android32_steam.c",
                  "src/android32_align.c", "src/android32_touch.c"]
PLATFORM_SOURCES = ["src/android32_setup.c"]
LIBRARIES = ["SDL3", "GLESv3", "EGL", "log", "android", "m", "dl", "z"]


def add_arguments(parser: Any) -> None:
    parser.add_argument("--android32-ndk", type=Path, default=None,
                        help="the Android NDK (default: ANDROID_NDK_HOME, or the newest under ANDROID_HOME)")
    parser.add_argument("--android32-sdl", type=Path, default=None,
                        help="SDL3 built for armeabi-v7a (default: build/android32/sdl3-build in this repository)")


def configure_inputs(port: Any) -> List[Path]:
    return [port.path("src"), *hud_configure_inputs()]


def _repository(port: Any) -> Path:
    return port.root.resolve().parent.parent


def _posix(path: Path) -> str:
    return path.as_posix()


def _ndk(sln: Any) -> Optional[Path]:
    given = getattr(sln.args, "android32_ndk", None) or os.environ.get("ANDROID_NDK_HOME")
    if given:
        return Path(given)
    home = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
    if home and (Path(home) / "ndk").is_dir():
        versions = sorted((Path(home) / "ndk").iterdir())
        return versions[-1] if versions else None
    return None


def _toolchain(ndk: Path) -> Optional[Path]:
    prebuilt = ndk / "toolchains" / "llvm" / "prebuilt"
    if not prebuilt.is_dir():
        return None
    found = sorted(prebuilt.iterdir())
    return found[0] if found else None


def generate(n: Any, sln: Any, port: Any) -> None:
    ndk = _ndk(sln)
    toolchain = _toolchain(ndk) if ndk else None
    sdl_build = Path(getattr(sln.args, "android32_sdl", None)
                     or _repository(port) / "build" / "android32" / "sdl3-build")
    sdl_source = _repository(port) / "build" / "android32" / "SDL3"
    if toolchain is None or not (sdl_build / "libSDL3.so").is_file() or not (sdl_source / "include" / "SDL3").is_dir():
        n.comment("32-bit Android (ninja android32): not generated, no NDK or no SDL3 built for armeabi-v7a "
                  "(port/android32/build.py)")
        return
    sdl_build, sdl_source = sdl_build.resolve(), sdl_source.resolve()
    config = _load_port_config()
    build_dir: Path = sln.build_dir / "android32"
    output = build_dir / "jniLibs" / "armeabi-v7a" / "libmain.so"
    cc = _posix(toolchain / "bin" / "clang")
    target = [f"--target={TRIPLE}"]
    linux_dir = sln.build_dir / "linux"

    n.comment("32-bit Android (ninja android32; port/android32/build.py)")
    n.variable("android32_cc", cc)
    n.rule(
        name="android32_cc",
        command=f"{compile_launcher(sln)}$android32_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="ANDROID32 CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="android32_link",
        command="$android32_cc $ldflags -o $out " + ("--rsp-quoting=windows " if os.name == "nt" else "")
        + "@$out.rsp $libs",
        description="ANDROID32 LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )

    android_sln = SimpleNamespace(**vars(sln))
    android_sln.game_browser = True
    generated = hud_assets_build(n, "android32", build_dir / "generated" / "hud_hires_assets.c")
    generated += ui_fonts_build(n, "android32", build_dir / "generated" / "ui_fonts.c", android_sln)
    units = Linux32Units(
        sln=android_sln, config=config, game_browser=True,
        semantics_header=linux_dir / "halo_msvc_semantics.h",
        platform_semantics_header=linux_dir / "platform_msvc_semantics.h",
        embedded_assets=generated, rule="android32_cc", target_flags=target,
        instruction_set=INSTRUCTION_SET,
        extra_flags=[*ABI_FLAGS, *CAPABILITIES, *console_defines(sln)],
        native_sources=[port.path(source) for source in NATIVE_SOURCES],
        platform_sources=[port.path(source) for source in PLATFORM_SOURCES],
        include_flags=[f"-I{_posix(port.path('include'))}", f"-I{_posix(sdl_source / 'include')}"],
    )
    lto_cflags, lto_ldflags = lto_flags(android_sln, build_dir / "thinlto-cache")
    objects = linux32_objects(n, units, build_dir / "obj", lto_cflags, [])
    n.build(
        outputs=output,
        rule="android32_link",
        inputs=objects,
        variables={
            "ldflags": " ".join([*target, *INSTRUCTION_SET, *lto_ldflags, "-fuse-ld=lld", "-shared", "-g",
                                 "-Wl,-Bsymbolic", "-Wl,--no-undefined", f"-L{_posix(sdl_build)}"]),
            "libs": " ".join(f"-l{library}" for library in LIBRARIES),
        },
        implicit=[sdl_build / "libSDL3.so"],
    )
    n.build(outputs="android32", rule="phony", inputs=output)
    n.newline()
