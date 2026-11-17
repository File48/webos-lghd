#!/bin/sh
# lghd boot hook: make LG's media pipelines load lghdaudiodec after every boot.
# Linked from /var/lib/webosbrew/init.d/60-lghd (Homebrew Channel runs it via run-parts).
#   - builds the RAM registry and bind-mounts it (lghd.sh build/install; nothing written to flash)
#   - stops idle starfish-media-pipeline processes that started before the install
#     (they read LG's registry once at start and would skip TrueHD/DTS)
#   - logs whether audiooutputd's digital output mode matches the TV setting
#   - bind-mounts the patched Litefin bundle (litefin-patch.sh) if the installed app matches
# Skip it for a boot:   touch /media/developer/lghd/disabled
# Remove it entirely:   rm -f /var/lib/webosbrew/init.d/60-lghd
DIR=/media/developer/lghd
LOG=/tmp/lghd_boot.log

[ -e $DIR/disabled ] && { echo "$(date +%T) disabled flag present, skipping" >> $LOG; exit 0; }

main() {
  echo "$(date +%T) boot hook start, uptime $(cut -d' ' -f1 /proc/uptime)s"
  # gst-inspect and the registry path need the system up; wait for /mnt/flash/data
  i=0; while [ ! -f /mnt/flash/data/gst_1_0_registry.arm.bin ] && [ $i -lt 60 ]; do sleep 2; i=$((i+1)); done
  [ -f $DIR/litefin-patch.sh ] && sh $DIR/litefin-patch.sh mount
  sh $DIR/lghd.sh build || { echo "build failed, not installing"; return 1; }
  sh $DIR/lghd.sh install || return 1
  T_INSTALL=$(cut -d' ' -f1 /proc/uptime)

  # Stop pre-started pipelines that are idle (only their base threads exist).
  # A pipeline that is playing has source/queue/multiqueue/... threads; leave it.
  for p in $(pidof starfish-media-pipeline); do
    others=$(for t in /proc/$p/task/*; do cat $t/comm 2>/dev/null; done | grep -vxE 'starfish-media-|tLibSystrim|tFragmentation' | wc -l)
    st=$(cut -d' ' -f22 /proc/$p/stat 2>/dev/null)
    age=$(awk -v u="$T_INSTALL" -v s="$st" 'BEGIN{printf "%.0f", u - s/100}')
    if [ "$others" -eq 0 ] && [ "$(grep -c lghdaudio /proc/$p/maps 2>/dev/null)" = 0 ]; then
      echo "stopping idle pre-install pipeline $p (age ${age}s)"; kill $p
    else
      echo "leaving pipeline $p (busy: $others extra threads)"
    fi
  done

  # Digital output mode check (one boot was seen to leave audiooutputd in "pcm" while the TV setting said passThrough).
  sleep 20
  want=$(luna-send -n 1 luna://com.webos.service.settings/getSystemSettings '{"category":"sound","keys":["soundOutputDigital"]}' 2>/dev/null | grep -o '"soundOutputDigital":"[^"]*"' | cut -d'"' -f4)
  have=$(timeout 5 luna-send -n 1 luna://com.webos.service.audiooutput/quality/getSettings '{"category":"soundOutputMode"}' 2>/dev/null | grep -o '"mode":"[^"]*"' | cut -d'"' -f4)
  if [ -n "$want" ] && [ "$want" != "$have" ]; then
    echo "WARNING: TV setting soundOutputDigital=$want but audiooutputd mode=$have (multichannel PCM needs passThrough). Toggle Digital Sound Output to fix."
  else
    echo "digital output mode OK: setting=$want audiooutputd=$have"
  fi
  echo "$(date +%T) boot hook done"
}

main >> $LOG 2>&1 &
exit 0
