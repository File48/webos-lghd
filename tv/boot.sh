#!/bin/sh
# lghd boot hook: make LG's media pipelines load lghdaudiodec after every boot.
# Linked from /var/lib/webosbrew/init.d/60-lghd (Homebrew Channel runs it via run-parts).
#   - builds the RAM registry and bind-mounts it (lghd.sh build/install; nothing written to flash)
#   - stops idle starfish-media-pipeline processes that started before the install
#     (they read LG's registry once at start and would skip TrueHD/DTS)
#   - sets Digital Sound Output to Auto (LG resets it to Pass Through at every boot)
#   - logs whether audiooutputd's digital output mode matches the TV setting
#   - bind-mounts the patched Litefin bundle (litefin-patch.sh) if the installed app matches
# Skip it for a boot:   touch /media/developer/lghd/disabled
# Keep LG's Digital Sound Output setting:   touch /media/developer/lghd/no-sound-auto
# Remove it entirely:   rm -f /var/lib/webosbrew/init.d/60-lghd
DIR=/media/developer/lghd
LOG=/tmp/lghd_boot.log

[ -e $DIR/disabled ] && { echo "$(date +%T) disabled flag present, skipping" >> $LOG; exit 0; }

main() {
  echo "$(date +%T) boot hook start, uptime $(cut -d' ' -f1 /proc/uptime)s"
  # LG resets Digital Sound Output to Pass Through at every boot (C2 / webOS 24), and Pass Through
  # sends Dolby Digital Plus Atmos from apps as stereo PCM. Auto keeps it and still gives multichannel
  # PCM from the decoder. The setting is per input, so set it for apps and every HDMI input.
  if [ ! -e $DIR/no-sound-auto ]; then
    for d in media hdmi1 hdmi2 hdmi3 hdmi4; do
      luna-send -n 1 luna://com.webos.settingsservice/setSystemSettings "{\"category\":\"sound\",\"settings\":{\"soundOutputDigital\":\"auto\"},\"dimension\":{\"audioInput\":\"$d\"}}" >/dev/null 2>&1
    done
    echo "set Digital Sound Output to auto (media, hdmi1-4)"
  fi
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
    echo "WARNING: TV setting soundOutputDigital=$want but audiooutputd mode=$have. Toggle Digital Sound Output to fix."
  else
    echo "digital output mode OK: setting=$want audiooutputd=$have"
  fi
  echo "$(date +%T) boot hook done"
}

main >> $LOG 2>&1 &
exit 0
