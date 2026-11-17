#!/bin/sh
# Remove lghdaudio from the TV.   sh uninstall.sh root@TV_IP [--purge]
# Without --purge the files stay in /media/developer/lghd but nothing is active,
# now or after reboot. A reboot alone also undoes every bind-mount.
set -eu
HOST=${1:?usage: uninstall.sh root@TV_IP [--purge]}
PURGE=${2:-}
D=/media/developer/lghd
ssh "$HOST" "rm -f /var/lib/webosbrew/init.d/60-lghd
  [ -f $D/litefin-patch.sh ] && sh $D/litefin-patch.sh umount
  [ -f $D/lghd.sh ] && sh $D/lghd.sh uninstall
  [ '$PURGE' = --purge ] && rm -rf $D && echo 'removed $D'
  echo 'uninstalled: fully close and relaunch your player app'"
