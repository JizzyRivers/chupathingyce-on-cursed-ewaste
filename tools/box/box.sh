#!/bin/sh
# box.sh run "cmd" | put local remote | get remote local: the Steam Link over ssh (PuTTY's plink and pscp).
# The root password is not in the repository: one line in ~/.chupathingyce-steamlink/box-password
# (or BOX_PASSWORD), the box's address in BOX_ADDRESS (192.168.2.80), and for a box whose
# host key PuTTY has not kept, its fingerprint in BOX_HOSTKEY (SHA256:...).
PW=${BOX_PASSWORD:-$(cat "$HOME/.chupathingyce-steamlink/box-password")}
BOX=root@${BOX_ADDRESS:-192.168.2.80}
P="/c/Program Files/PuTTY"
HK=${BOX_HOSTKEY:+-hostkey $BOX_HOSTKEY}
case "$1" in
run) "$P/plink" -ssh -batch $HK -pw "$PW" "$BOX" "$2" ;;
put) "$P/pscp" -batch $HK -scp -q -pw "$PW" "$2" "$BOX:$3" ;;
get) "$P/pscp" -batch $HK -scp -q -pw "$PW" "$BOX:$2" "$3" ;;
*) echo "box.sh run|put|get"; exit 1 ;;
esac
