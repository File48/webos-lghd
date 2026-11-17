#!/bin/sh
# lghd probe: run on the TV, then start playback in Litefin within ~10 s.
# Records, for one real playback through LG's own media pipeline:
#   - every luna setQualityAttribute call (who sends what, with which codec/channel)
#   - each starfish-media-pipeline process: argv, GST/LD env, registry mount, whether lghdaudio is mapped
#   - the eARC output type once per second
#   - starfish/uMediaServer/audio lines from the system log
# Read-only: writes only under /tmp/lghd_probe. Duration: $1 seconds (default 90).
DUR=${1:-90}
OUT=/tmp/lghd_probe
rm -rf $OUT; mkdir -p $OUT
pkill -x ls-monitor 2>/dev/null
LOGLINES=$(wc -l < /tmp/var/log/messages 2>/dev/null || echo 0)
nohup setsid timeout $DUR ls-monitor > $OUT/lsmon.raw 2>&1 < /dev/null &
T0=$(date +%s)
echo "probe running ${DUR}s -> start playback now"
seen=" "
while [ $(( $(date +%s) - T0 )) -lt $DUR ]; do
  t=$(( $(date +%s) - T0 ))
  echo "$t $(amixer -c 0 cget numid=91 | tail -1 | sed 's/.*values=//') onoff=$(amixer -c 0 cget numid=90 | tail -1 | sed 's/.*values=//')" >> $OUT/earc.txt
  for p in $(pidof starfish-media-pipeline); do
    m=$(grep -c lghdaudio /proc/$p/maps 2>/dev/null)
    echo "$t pid=$p lghd_maps=$m" >> $OUT/pids.txt
    case "$seen" in *" $p "*) continue ;; esac
    seen="$seen$p "
    {
      echo "== pid $p first seen at t=$t"
      echo "-- cmdline:"; tr '\0' ' ' < /proc/$p/cmdline; echo
      echo "-- env (GST/LD/MEDIA/PIPELINE):"; tr '\0' '\n' < /proc/$p/environ | grep -E '^(GST|LD_|MEDIA|PIPELINE|UMS|STARFISH)'
      echo "-- registry mounts seen by process:"; grep gst_1_0 /proc/$p/mountinfo
      echo "-- stat starttime (field 22, jiffies):"; cut -d' ' -f22 /proc/$p/stat
    } >> $OUT/proc.txt 2>&1
  done
  sleep 1
done
sleep 1
grep -E "setQualityAttribute" $OUT/lsmon.raw | grep -E "audioInputInfo" | cut -c1-900 > $OUT/audio_calls.txt
grep "/quality/setQualityAttribute" $OUT/lsmon.raw | grep -o 'categoryInfo":{[^}]*}' | sort | uniq -c > $OUT/audio_summary.txt
tail -n +$((LOGLINES + 1)) /tmp/var/log/messages 2>/dev/null | grep -iE "starfish|umediaserver|umimedia|audiooutputd|audiod|lghd|truehd|mlp|NL_SOUND|error" | cut -c1-400 > $OUT/syslog.txt
echo "== summary"; cat $OUT/audio_summary.txt
echo "== eARC changes"; awk '{k=$2" "$3} k!=last {print; last=k}' $OUT/earc.txt
echo "== lghd mapped in any pipeline:"; awk '$3!="lghd_maps=0"' $OUT/pids.txt | head -5
echo "full output: $OUT (proc.txt, audio_calls.txt, syslog.txt, lsmon.raw)"
