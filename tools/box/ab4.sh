#!/bin/sh
# ab4.sh "ENV=..." map... : the maps with that launch.env line (or none for "-")
[ "$1" = "-" ] && : > /tmp/trial/launch.env || printf 'export %s\n' "$1" > /tmp/trial/launch.env
shift
sh /tmp/trial/sweep.sh 110 "$@" | cut -d'|' -f1,3,4,5
: > /tmp/trial/launch.env
