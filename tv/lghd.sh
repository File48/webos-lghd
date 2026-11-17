#!/bin/sh
# lghd.sh - install/test/rollback for the lghdaudiodec GStreamer plugin
# Nothing here writes to system partitions. "install" bind-mounts a RAM copy
# of the registry over LG's; a reboot or "uninstall" undoes it.
#
#   sh lghd.sh build      # build test registry in /tmp, verify nothing lost
#   sh lghd.sh test FILE  # play FILE (mka/mkv with TrueHD/DTS) via LG sink using test registry
#   sh lghd.sh install    # bind-mount test registry over LG's (live for new playbacks)
#   sh lghd.sh uninstall  # remove bind-mount, restore eARC default
#   sh lghd.sh status
set -u
DIR=/media/developer/lghd
PLUG=$DIR/plugins
ORIG=/mnt/flash/data/gst_1_0_registry.arm.bin
NEW=/tmp/lghd_registry.arm.bin
BASEPATH=/usr/lib/gstreamer-1.0:/mnt/lg/res/lglib/gstreamer-1.0
export LD_PRELOAD=/usr/lib/libsystrim.so.3

feats() { # list "plugin:feature" from a registry
  GST_REGISTRY_1_0=$1 GST_REGISTRY_UPDATE=no GST_PLUGIN_PATH_1_0=$2 \
    gst-inspect-1.0 2>/dev/null | grep -E '^[a-z0-9_]+: ' | sed 's/: */:/' | cut -d: -f1,2 | sort
}

# /mnt/flash/data resolves to /mnt/lg/flash/data, and /proc/mounts lists the real path.
# The bind also shows on peer mounts (/mnt/lg/user, /mnt/lg/cmn_data) via mount propagation.
ORIG_REAL=$(readlink -f $ORIG)
mounted() { grep -q " $ORIG_REAL " /proc/mounts; }

case "${1:-}" in
build)
  [ -f $PLUG/libgstlghdaudio.so ] || { echo "missing $PLUG/libgstlghdaudio.so"; exit 1; }
  command -v gst-inspect-1.0 >/dev/null || { echo "no gst-inspect-1.0 on this TV"; exit 1; }
  mounted && { echo "registry already bind-mounted; uninstall first"; exit 1; }
  # start from LG's registry so cached entries for unchanged plugins are kept
  cp $ORIG $NEW || exit 1
  echo "== scanning (only new/changed plugins get loaded)"
  GST_REGISTRY_1_0=$NEW GST_REGISTRY_UPDATE=yes GST_PLUGIN_PATH_1_0=$BASEPATH:$PLUG \
    gst-inspect-1.0 lghdaudiodec > /tmp/lghd_inspect.txt 2>&1
  if ! grep -q "lghdaudiodec" /tmp/lghd_inspect.txt || grep -q "No such element" /tmp/lghd_inspect.txt; then
    echo "FAIL: element not registered"; cat /tmp/lghd_inspect.txt; rm -f $NEW; exit 1
  fi
  grep -E "Rank|Long-name|audio/x-" /tmp/lghd_inspect.txt
  feats $ORIG $BASEPATH > /tmp/lghd_f_orig.txt
  feats $NEW $BASEPATH:$PLUG > /tmp/lghd_f_new.txt
  LOST=$(grep -vxFf /tmp/lghd_f_new.txt /tmp/lghd_f_orig.txt)
  ADDED=$(grep -vxFf /tmp/lghd_f_orig.txt /tmp/lghd_f_new.txt)
  echo "== features: orig=$(wc -l < /tmp/lghd_f_orig.txt) new=$(wc -l < /tmp/lghd_f_new.txt)"
  echo "added: $ADDED"
  if [ -n "$LOST" ]; then echo "FAIL: features lost vs LG registry:"; echo "$LOST"; rm -f $NEW; exit 1; fi
  [ "$(wc -l < /tmp/lghd_f_orig.txt)" -gt 20 ] || { echo "FAIL: could not read original feature list"; rm -f $NEW; exit 1; }
  echo "OK: $NEW ready (nothing lost)"
  ;;
test)
  F=${2:?file}; [ -f $NEW ] || { echo "run build first"; exit 1; }
  pkill -x gst-launch-1.0 2>/dev/null
  PID=_lghdtest0001
  PI="\"pipelineId\":\"$PID\",\"streamType\":\"umimedia\",\"contentType\":\"movie\",\"sourceType\":\"MEDIA\",\"resource\":\"ADEC0\",\"sourceAttribute\":\"MEDIA\",\"gameInfo\":{\"isGameMode\":false}"
  # Every setAudioInputInfo goes straight to audiooutputd with activeState:true.
  # libumi gates updateAudioCategoryInfo -> updateCodecImmersiveFormat on one
  # global flag (umiTVAudio+0x7d, default true) that only activeState sets.
  # audiooutputd does not reply when its schema rejects a payload, hence timeout.
  info() {
    timeout 5 luna-send -n 1 luna://com.webos.service.audiooutput/quality/setQualityAttribute \
      "{\"categoryType\":\"audioInputInfo\",\"requestType\":\"setAudioInputInfo\",\"activeState\":true,\"pipelineInfo\":{$PI,\"reset\":$1},\"categoryInfo\":{\"codec\":\"$2\",\"channel\":$3,\"immersive\":\"none\"}}" \
      >> /tmp/lghd_active.txt 2>&1 || echo "no reply: info $1 $2 $3 (schema reject or denied)" >> /tmp/lghd_active.txt
  }
  : > /tmp/lghd_active.txt
  echo "== routing audio first, then feeding $F for 30s"
  info true none 0
  luna-send -n 1 luna://com.webos.service.audiooutput/audio/connect \
    '{"source":"ADEC0","sink":"MAIN","pipelineAttribute":{"pipelinePurpose":null,"sourceType":"MEDIA","streamType":"umimedia"}}' >/dev/null
  # Simulate LG: report a non-PCM codec, and leave switching to PCM to the plugin
  # (v2 sends pcm/6 itself; LGHD_PIPELINE_ID stands in for starfish's argv).
  info false aac 2
  echo "== eARC before render: $(amixer -c 0 cget numid=91 | tail -1)"
  LOGSTART=$(cat /tmp/lghd.log 2>/dev/null | wc -l)
  ( sleep 3; cat "$F" ) | LGHD_PIPELINE_ID=$PID GST_REGISTRY_1_0=$NEW GST_REGISTRY_UPDATE=no GST_PLUGIN_PATH_1_0=$BASEPATH:$PLUG \
  GST_DEBUG=${LGDBG:-2,lghdaudiodec:5,lgaudiosink:4} timeout 30 gst-launch-1.0 \
    fdsrc fd=0 ! matroskademux ! queue ! lghdaudiodec ! audioconvert ! \
    audiosink ${SINKX:-} index=0 resource-info="res,audio-port=(int)0" > /tmp/lghd_test.log 2>&1 &
  sleep 8
  echo "== at ~8s: eARC=$(amixer -c 0 cget numid=91 | tail -1)"
  wait 2>/dev/null
  luna-send -n 1 luna://com.webos.service.audiooutput/audio/disconnect '{"source":"ADEC0","sink":"MAIN"}' >/dev/null
  info true none 0   # let audiooutputd restore the output itself; no direct ALSA write
  echo "== after reset: eARC=$(amixer -c 0 cget numid=91 | tail -1)"
  cat /tmp/lghd_active.txt
  echo "== plugin log (/tmp/lghd.log, this run):"; tail -n +$((LOGSTART + 1)) /tmp/lghd.log
  grep -vE "Chapters" /tmp/lghd_test.log | grep -E "lghd|ERROR|render_set_caps|eARC" | head -20
  echo "full log: /tmp/lghd_test.log"
  ;;
install)
  [ -f $NEW ] || { echo "run build first"; exit 1; }
  mounted && { echo "already installed"; exit 0; }
  mount --bind $NEW $ORIG && echo "installed (new playbacks use it). rollback: sh $0 uninstall"
  ;;
uninstall)
  mounted && umount $ORIG && echo "registry bind-mount removed"
  amixer -c 0 cset numid=91 2,1,0,0,1 >/dev/null && echo "eARC output type reset to default"
  ;;
status)
  mounted && echo "INSTALLED (bind-mounted)" || echo "not installed"
  amixer -c 0 cget numid=91 | tail -1
  ;;
*) sed -n '2,12p' $0 ;;
esac
