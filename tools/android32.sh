#!/bin/sh
# android32.sh: build, stage, package and install the 32-bit Android port on a device (default: the S905W2 box),
# then start it and show the log. Needs ANDROID_HOME (the SDK with the NDK), JAVA_HOME (17) and LLVM, ninja, cmake on PATH.
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd -W)
dev=${ANDROID32_DEVICE:-192.168.2.213:5555}
(cd "$repo/chupathingyce" && ninja android32) || exit 1
python "$repo/port/android32/stage.py" || exit 1
(cd "$repo/port/android32/app" && ./gradlew --console=plain -q assembleDebug) || exit 1
apk=$repo/port/android32/app/app/build/outputs/apk/debug/app-debug.apk
MSYS_NO_PATHCONV=1 adb -s "$dev" install -r "$apk" || exit 1
MSYS_NO_PATHCONV=1 adb -s "$dev" logcat -c
MSYS_NO_PATHCONV=1 adb -s "$dev" shell am start -n dev.warman.chupathingyce32/.ChupathingyActivity
