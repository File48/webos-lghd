# Build environment for libgstlghdaudio.so (works on macOS/Linux/Windows with Docker).
#   docker build -t lghd-build .
#   docker run --rm -v "$PWD":/w -w /w lghd-build sh -c 'sh build/ffmpeg.sh && sh build/build.sh'
# Run build/fetch-sysroot.sh on the host first (it needs ssh access to the TV).
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      gcc-arm-linux-gnueabi binutils-arm-linux-gnueabi make git ca-certificates nasm pkg-config \
      libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libglib2.0-dev libasound2-dev \
    && rm -rf /var/lib/apt/lists/*
