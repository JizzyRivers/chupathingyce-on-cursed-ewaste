#!/bin/sh
# deploy.sh: the built game (chupathingyce/build/steamlink/halo), stripped, onto the box as the app's
# binary, with the box-side test scripts in /tmp/trial (which a reboot of the box empties).
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
"/c/Program Files/LLVM/bin/llvm-strip" -o "$repo/build/halo-stripped" "$repo/chupathingyce/build/steamlink/halo" || exit 1
"$here/box.sh" run 'pidof halo >/dev/null && { echo "the game is running on the box: not replaced"; exit 1; }; mkdir -p /tmp/trial; [ -f /tmp/trial/launch.env ] || : > /tmp/trial/launch.env' || exit 1
for f in sweep.sh mem.sh ab4.sh; do "$here/box.sh" put "$here/$f" /tmp/trial/$f; done
"$here/box.sh" put "$repo/build/halo-stripped" /home/apps/chupathingyce/halo && "$here/box.sh" run 'chmod +x /home/apps/chupathingyce/halo; md5sum /home/apps/chupathingyce/halo'
