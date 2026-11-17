#!/bin/sh
# Copy the shared libraries the plugin links against from YOUR TV into ./sysroot.
# Nothing from LG is shipped in this repo; the build links against the TV's own
# glibc / GStreamer / GLib / ALSA so symbol versions match exactly.
#
#   sh build/fetch-sysroot.sh root@TV_IP        (or an ssh alias, e.g. "tv")
set -eu
HOST=${1:?usage: fetch-sysroot.sh root@TV_IP}
OUT=${2:-sysroot}
mkdir -p "$OUT"
ssh "$HOST" 'cd / && tar cf - \
  lib/ld-linux.so.3 lib/libc.so.6 lib/libm.so.6 lib/libpthread.so.0 lib/libdl.so.2 lib/librt.so.1 \
  usr/lib/libgstreamer-1.0.so.0* usr/lib/libgstbase-1.0.so.0* usr/lib/libgstaudio-1.0.so.0* \
  usr/lib/libgsttag-1.0.so.0* usr/lib/libglib-2.0.so.0* usr/lib/libgobject-2.0.so.0* \
  usr/lib/libgmodule-2.0.so.0* usr/lib/libffi.so.8* usr/lib/libpcre.so.1* usr/lib/libasound.so.2* \
  usr/lib/libpthreadpool.so' | tar xf - -C "$OUT"
echo "sysroot in $OUT:"
ls "$OUT/lib" "$OUT/usr/lib"
