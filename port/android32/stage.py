"""stage.py: build/android32/stage/jniLibs/armeabi-v7a, the libraries the app
(app/) packs: the game's libmain.so, stripped (the build keeps its symbols in
build/android32 for llvm-symbolizer), and SDL3's libSDL3.so. Then
``gradlew assembleDebug`` in port/android32/app makes the APK."""
import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build" / "android32"
OUT = BUILD / "stage" / "jniLibs" / "armeabi-v7a"


def main():
    ndk = Path(os.environ.get("ANDROID_NDK_HOME") or sorted((Path(os.environ["ANDROID_HOME"]) / "ndk").iterdir())[-1])
    strip = next((ndk / "toolchains" / "llvm" / "prebuilt").iterdir()) / "bin" / "llvm-strip"
    shutil.rmtree(BUILD / "stage", ignore_errors=True)
    OUT.mkdir(parents=True)
    libmain = ROOT / "chupathingyce" / "build" / "android32" / "jniLibs" / "armeabi-v7a" / "libmain.so"
    if os.environ.get("ANDROID32_KEEP_SYMBOLS"):
        # (for simpleperf on the device: the functions' names)
        shutil.copy2(libmain, OUT / "libmain.so")
    else:
        subprocess.run([str(strip), "--strip-unneeded", "-o", str(OUT / "libmain.so"), str(libmain)], check=True)
    subprocess.run([str(strip), "--strip-unneeded", "-o", str(OUT / "libSDL3.so"), str(BUILD / "sdl3-build" / "libSDL3.so")],
                   check=True)
    for library in sorted(OUT.iterdir()):
        print(f"{library.name}: {library.stat().st_size} bytes")


if __name__ == "__main__":
    main()
