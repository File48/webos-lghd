#!/bin/sh
# Build libgstlghdaudio.so for LG webOS 24 (armel softfp, glibc 2.35, GStreamer 1.18.5).
# Run from the repo root after build/ffmpeg.sh and build/fetch-sysroot.sh.
# Headers: host GStreamer/GLib/ALSA dev packages + a 32-bit ARM glibconfig.h.
# Links against the TV's own libraries (./sysroot) with --no-undefined, so any
# symbol the TV doesn't have fails the link instead of failing on the TV.
set -eu
TOP=$(pwd)
SYSROOT=${SYSROOT:-$TOP/sysroot}
FF=${FF:-$TOP/ffout}
OUT=${1:-$TOP/libgstlghdaudio.so}
[ -f "$SYSROOT/lib/libc.so.6" ] || { echo "no sysroot: run sh build/fetch-sysroot.sh root@TV_IP"; exit 1; }
[ -f "$FF/lib/libavcodec.a" ] || { echo "no FFmpeg: run sh build/ffmpeg.sh"; exit 1; }

W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
mkdir -p "$W/inc" "$W/lib"
cp build/glibconfig-armel32.h "$W/inc/glibconfig.h"
for d in alsa glib-2.0 gstreamer-1.0; do ln -s /usr/include/$d "$W/inc/$d"; done
# unversioned link names -> TV libraries
for f in "$SYSROOT"/lib/* "$SYSROOT"/usr/lib/*; do ln -sf "$f" "$W/lib/$(basename "$f")"; done
for n in gstreamer-1.0 gstbase-1.0 gstaudio-1.0 gsttag-1.0 glib-2.0 gobject-2.0; do
  ln -sf lib$n.so.0 "$W/lib/lib$n.so"; done
ln -sf libasound.so.2 "$W/lib/libasound.so"

arm-linux-gnueabi-gcc -mcpu=cortex-a7 -mfloat-abi=softfp -mfpu=neon -O2 -fPIC -shared \
  -Wall -Wextra -Wno-unused-parameter -o "$OUT" src/gstlghdaudiodec.c \
  -I"$W/inc" -I"$W/inc/gstreamer-1.0" -I"$W/inc/glib-2.0" -I"$FF/include" \
  -L"$W/lib" -Wl,-rpath-link,"$W/lib" -lgstaudio-1.0 -lgstbase-1.0 -lgstreamer-1.0 -lgobject-2.0 \
  -lglib-2.0 -lasound "$FF/lib/libavcodec.a" "$FF/lib/libavutil.a" \
  "$W/lib/libm.so.6" "$W/lib/libpthread.so.0" "$W/lib/libc.so.6" "$W/lib/ld-linux.so.3" \
  -Wl,--no-undefined -Wl,--exclude-libs,ALL -Wl,-Bsymbolic -Wl,--as-needed
arm-linux-gnueabi-strip --strip-unneeded "$OUT"
MAX=$(arm-linux-gnueabi-objdump -T "$OUT" | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)
echo "max glibc symbol: $MAX (must be <= GLIBC_2.35)"
echo "NEEDED: $(arm-linux-gnueabi-readelf -d "$OUT" | grep NEEDED | grep -oE '\[[^]]+\]' | tr '\n' ' ')"
md5sum "$OUT"
