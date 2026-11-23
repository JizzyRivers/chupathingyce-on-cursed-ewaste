"""package.py: the 32-bit Android port's APK, signed, in build/android32/release/.

    python port/android32/package.py

Builds the game (ninja android32, as a release of the game's version: HALO_VERSION and
HALO_RELEASE_BUILD), stages its libraries stripped (stage.py), and has Gradle make the release APK,
signed with the key in ~/.chupathingyce-steamlink/android32-keystore.properties (storeFile,
storePassword, keyAlias, keyPassword: never in a repository; keep a copy, since an app is updated
only by a build signed with the key it was installed with). Prints the APK's SHA-256.

Needs ANDROID_HOME (the SDK with the NDK), JAVA_HOME (17), and LLVM, ninja and cmake on PATH, as
tools/android32.sh does.
"""
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GAME = ROOT / "chupathingyce"
OUT = ROOT / "build" / "android32" / "release"
APP = ROOT / "port" / "android32" / "app"
KEYSTORE = Path.home() / ".chupathingyce-steamlink" / "android32-keystore.properties"


def run(*command, **kwargs):
    subprocess.run([str(part) for part in command], check=True, **kwargs)


def main():
    version = (GAME / "VERSION").read_text(encoding="utf-8").strip()
    if not KEYSTORE.is_file():
        sys.exit(f"no signing key at {KEYSTORE}")
    environment = dict(os.environ, HALO_VERSION=version, HALO_RELEASE_BUILD="1",
                       ANDROID32_KEYSTORE_PROPERTIES=str(KEYSTORE))
    environment.pop("ANDROID32_KEEP_SYMBOLS", None)
    run(sys.executable, "configure.py", "--release", "--console-port", "../port/android32", cwd=GAME, env=environment)
    run("ninja", "android32", cwd=GAME, env=environment)
    run(sys.executable, ROOT / "port" / "android32" / "stage.py", env=environment)
    gradle = APP / ("gradlew.bat" if os.name == "nt" else "gradlew")
    run(gradle, "--console=plain", "-q", "assembleRelease", cwd=APP, env=environment)
    built = APP / "app" / "build" / "outputs" / "apk" / "release" / "app-release.apk"
    if not built.is_file():
        sys.exit("Gradle made no signed release APK (app-release.apk): is the key readable?")
    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    apk = OUT / f"chupathingyce-android32-{version}.apk"
    shutil.copy2(built, apk)
    digest = hashlib.sha256(apk.read_bytes()).hexdigest()
    (OUT / (apk.name + ".sha256")).write_text(f"{digest}  {apk.name}\n", encoding="utf-8")
    print(f"{apk}: {apk.stat().st_size} bytes\nsha256 {digest}")


if __name__ == "__main__":
    main()
