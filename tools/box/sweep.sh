#!/bin/sh
# sweep.sh seconds map... : each map (its scenario path, levels\...) loaded by init.txt with a true launch,
# run for the seconds, then: the layer's frame times, the game's peak memory, and how it ended
stick=/mnt/chupathingyce-usb
app=/home/apps/chupathingyce
LIMIT=$1; shift
for map in "$@"; do
	mkdir -p $stick; mount | grep -q "$stick" || mount -o noatime /dev/block/sda1 $stick
	printf 'map_name %s\n' "$map" | tr '/' '\\' > $stick/chupathingyce/init.txt
	cp /tmp/trial/launch.env $app/launch.env
	sync; umount $stick
	echo "$app/run.sh" > /tmp/launch_cmdline.txt
	killall shell
	i=0; peak=0
	while [ $i -lt $LIMIT ]; do
		sleep 5; i=$((i+5))
		p=$(pidof halo)
		if [ -n "$p" ]; then
			r=$(awk '/VmHWM/ {print $2}' /proc/$p/status 2>/dev/null)
			[ -n "$r" ] && peak=$r
		elif [ $i -gt 30 ]; then
			break
		fi
	done
	alive=$(pidof halo)
	[ -n "$alive" ] && { kill $alive; sleep 6; }
	mkdir -p $stick; mount | grep -q "$stick" || mount -o noatime /dev/block/sda1 $stick
	rm -f $app/launch.env $stick/chupathingyce/init.txt
	log=$stick/chupathingyce/run.log
	times=$(grep -E 'frames .* took' $log | tail -6 | head -5 | sed -E 's/.*took ([0-9.]+) ms.*draws.*/\1/; s/.*took ([0-9.]+) ms.*/\1/' | tr '\n' ' ')
	draws=$(grep -E 'frames .* took' $log | tail -2 | head -1 | sed -E 's/.*calls: ([0-9]+) draws.*/\1/')
	end="ran"; [ -z "$alive" ] && end="ENDED at ${i}s"
	crash=$(grep -E 'crash report|segmentation|Assertion|halt' $log | head -2 | cut -c1-120 | tr '\n' ' ')
	echo "$map | $end | peak $((peak/1024)) MB | ms/frame: $times| draws $draws | $crash"
	name=$(echo "$map" | tr '/\' '__')
	cp $log /mnt/scratch/sweep-$name.log
	sync; umount $stick
done
