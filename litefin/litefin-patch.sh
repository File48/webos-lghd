#!/bin/sh
# litefin-patch.sh - bind-mount patched Litefin 2.0.20 bundle files over the installed app.
# Make the patched files first with litefin/make-patch.py and copy them to $PDIR.
# RAM-level only: the app itself is never modified; reboot or "umount" undoes it.
# Each file is mounted only if BOTH the installed file and the patched copy have the
# expected md5, so a Litefin update simply turns the patch off.
#   sh litefin-patch.sh mount | umount | status
set -u
APP=/media/developer/apps/usr/palm/applications/org.litefin.app
PDIR=/media/developer/lghd/litefin
# name  installed-md5 (2.0.20)  patched-md5
FILES="main-5d7218db.js:5e97eccfcf0ba768d21fcd20c1e8e7c4:352d968667fe96551c6635ce11948c6a
main-37bbcbe8.js:24b7ac5527c0f03f65ce36ede7f4f63b:b75b606d3fbb0e2f7948680b7227b8f8"

md5() { md5sum < "$1" | cut -c1-32; }
mounted() { grep -q " $(readlink -f "$1") " /proc/mounts; }

case "${1:-}" in
mount)
  for e in $FILES; do
    n=${e%%:*}; r=${e#*:}; om=${r%%:*}; pm=${r#*:}
    o=$APP/js/$n; p=$PDIR/$n
    if mounted "$o"; then echo "$n already mounted"
    elif [ ! -f "$p" ] || [ "$(md5 "$p")" != "$pm" ]; then echo "SKIP $n: patched copy missing or wrong md5"
    elif [ "$(md5 "$o")" != "$om" ]; then echo "SKIP $n: installed file changed (Litefin updated?)"
    else mount --bind "$p" "$o" && echo "mounted patched $n"; fi
  done
  echo "fully close and relaunch Litefin to load it"
  ;;
umount)
  for e in $FILES; do n=${e%%:*}; o=$APP/js/$n
    mounted "$o" && umount "$o" && echo "unmounted $n"; done
  echo "fully close and relaunch Litefin"
  ;;
status)
  for e in $FILES; do n=${e%%:*}; o=$APP/js/$n
    mounted "$o" && echo "$n: PATCH ACTIVE" || echo "$n: not mounted"; done
  ;;
*) sed -n '2,8p' "$0" ;;
esac
