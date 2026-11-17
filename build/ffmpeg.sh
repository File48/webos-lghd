#!/bin/sh
# Build the minimal static FFmpeg used by the plugin: TrueHD/MLP and DTS decoders only.
# LGPL-2.1-or-later configuration (no --enable-gpl, no --enable-nonfree).
#   sh build/ffmpeg.sh            -> ./ffmpeg-src, ./ffout
set -eu
TOP=$(pwd)
[ -d ffmpeg-src ] || git clone --depth 1 --branch n7.1 https://github.com/FFmpeg/FFmpeg.git ffmpeg-src
cd ffmpeg-src
./configure --enable-cross-compile --cross-prefix=arm-linux-gnueabi- --arch=arm --cpu=cortex-a7 \
  --target-os=linux --enable-pic --disable-everything --enable-decoder='truehd,mlp,dca' \
  --enable-parser='mlp,dca' --enable-static --disable-shared --disable-programs --disable-doc \
  --disable-avdevice --disable-avformat --disable-swscale --disable-postproc --disable-avfilter \
  --disable-swresample --disable-network --disable-autodetect --disable-iconv \
  --extra-cflags='-mfloat-abi=softfp -mfpu=neon' --prefix="$TOP/ffout"
make -j"$(nproc)"
make install
grep -E 'CONFIG_GPL |CONFIG_NONFREE ' config.h   # both must be 0
