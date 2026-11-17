#!/bin/sh
# Install lghdaudio on a rooted LG webOS TV from your computer.
#   sh install.sh root@TV_IP [path/to/libgstlghdaudio.so]
# Puts everything in /media/developer/lghd (developer partition), builds a RAM copy of
# the GStreamer registry and bind-mounts it over LG's, and links the boot hook into
# /var/lib/webosbrew/init.d. No system partition is written.
set -eu
HOST=${1:?usage: install.sh root@TV_IP [libgstlghdaudio.so]}
SO=${2:-libgstlghdaudio.so}
D=/media/developer/lghd
[ -f "$SO" ] || { echo "missing $SO (build it first, see README)"; exit 1; }
cd "$(dirname "$0")"

ssh "$HOST" "mkdir -p $D/plugins $D/litefin"
scp "$SO" "$HOST:$D/plugins/.tmp"
scp tv/lghd.sh tv/boot.sh tv/probe.sh litefin/litefin-patch.sh "$HOST:$D/"
scp tv/lghd.conf "$HOST:$D/lghd.conf.default"
for f in litefin/out/main-*.js; do
  [ -f "$f" ] && scp "$f" "$HOST:$D/litefin/"
done

ssh "$HOST" "cd $D && set -e
  [ -f lghd.conf ] || cp lghd.conf.default lghd.conf
  sh lghd.sh uninstall >/dev/null 2>&1 || true
  mv plugins/.tmp plugins/libgstlghdaudio.so
  md5sum plugins/libgstlghdaudio.so
  sh lghd.sh build
  sh lghd.sh install
  sh litefin-patch.sh mount || true
  mkdir -p /var/lib/webosbrew/init.d
  ln -sf $D/boot.sh /var/lib/webosbrew/init.d/60-lghd
  # pre-started pipelines read the registry once at start; stop the idle ones
  for p in \$(pidof starfish-media-pipeline); do
    n=\$(for t in /proc/\$p/task/*; do cat \$t/comm; done | grep -vxcE 'starfish-media-|tLibSystrim|tFragmentation' || true)
    [ \"\$n\" -eq 0 ] && kill \$p && echo \"stopped idle pipeline \$p\"
  done
  echo 'done: fully close and relaunch your player app'"
