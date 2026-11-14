#!/bin/sh
# mem.sh seconds map: the map run for the seconds, then where the game's memory is (smaps, by mapping)
stick=/mnt/chupathingyce-usb
app=/home/apps/chupathingyce
mkdir -p $stick; mount | grep -q "$stick" || mount -o noatime /dev/block/sda1 $stick
printf 'map_name %s\n' "$2" | tr '/' '\' > $stick/chupathingyce/init.txt
: > $app/launch.env
sync; umount $stick
echo "$app/run.sh" > /tmp/launch_cmdline.txt
killall shell
sleep $1
p=$(pidof halo)
[ -z "$p" ] && { echo "halo gone"; exit 1; }
grep -E "VmHWM|VmRSS|VmData|VmSize|Threads" /proc/$p/status | tr -s '\t ' ' ' | tr '\n' ';'; echo
awk '{ split($1, r, "-"); s = (("0x" r[2]) + 0 - (("0x" r[1]) + 0)) / 1024; print s, $1, $2, $6 }' /proc/$p/maps > /mnt/scratch/maps.txt; cp /proc/$p/maps /mnt/scratch/maps-raw.txt; cat /proc/$p/statm
grep -E "MemFree|^Cached|Buffers|Shmem|Slab|Mapped" /proc/meminfo | tr -s ' ' | tr '\n' ';'; echo
kill $p; sleep 6
mount | grep -q "$stick" || mount -o noatime /dev/block/sda1 $stick
rm -f $app/launch.env $stick/chupathingyce/init.txt; sync; umount $stick
