# webos-lghd

A GStreamer plugin for **rooted** LG webOS TVs. In apps that use the TV's own media pipeline, it decodes **Dolby TrueHD** and **DTS / DTS-HD MA** on the TV and sends them over eARC as **lossless multichannel PCM (5.1)**. It also keeps **Dolby Vision** working when an MKV is direct-played.

It was built for one setup: Jellyfin → [Litefin](https://github.com/MoazSalem/litefin) on an LG C2 → Sonos soundbar on eARC. Without it, the TV can't play TrueHD from apps, so Jellyfin transcodes it to EAC3. Dolby Vision MKVs also play as HDR10, or get remuxed.

| | Stock | With lghd |
|---|---|---|
| TrueHD in MKV (direct play) | not supported → server transcodes to EAC3 | decoded on the TV → 5.1 multichannel PCM over eARC |
| DTS / DTS-HD MA in MKV | no sound (LG's demuxer labels it `audio/x-unknown`, nothing decodes it) | decoded on the TV → 5.1 multichannel PCM |
| Dolby Vision (profile 8) in MKV | HDR10 base layer | Dolby Vision |

> **Scope.** Tested only on an **LG OLED55C24LA (C2), webOS 24 / SDK 9.2.2**, with a Sonos soundbar on eARC and Litefin 2.0.20. Other models and firmware may differ in ways that matter: resource names, ALSA controls, registry paths. It needs root (e.g. via [webosbrew](https://www.webosbrew.org/) / Homebrew Channel). Use at your own risk.

## What it is

- **`lghdaudiodec`** decodes TrueHD and DTS with FFmpeg (LGPL build: the TrueHD/MLP/DCA decoders only) into the 8-slot PCM layout LG's DSP expects. It then tells LG's `audiooutputd` that the stream is now PCM, so eARC switches to multichannel PCM. Without that step the TV picks stereo.
- **`lghddvtag`** sits after LG's MKV demuxer. It spots Dolby Vision RPU NAL units in the first frame and adds the `dolby-vision` caps that LG's DV decoder needs. LG's MP4 demuxer adds these itself; its MKV demuxer doesn't.
- **Install method.** The plugin is added to a RAM copy of the GStreamer registry, which is bind-mounted over LG's. Nothing is written to system partitions. A reboot, or `uninstall.sh`, undoes it, and a Homebrew Channel boot hook re-applies it at each boot.
- **Litefin patches** (optional, Litefin 2.0.20 only) are applied as bind mounts. The app itself is never modified. See [Litefin](#litefin).

## Build

The build needs Linux with an ARM cross-compiler, or Docker on macOS/Windows. It links against your TV's own libraries, so nothing from LG is distributed here.

```sh
git clone https://github.com/File48/webos-lghd && cd webos-lghd
sh build/fetch-sysroot.sh root@TV_IP            # copies the TV's libs into ./sysroot (needs ssh)
docker build -t lghd-build .
docker run --rm -v "$PWD":/w -w /w lghd-build sh -c 'sh build/ffmpeg.sh && sh build/build.sh'
```

On Ubuntu 24.04 you can build without Docker. Install `gcc-arm-linux-gnueabi libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libglib2.0-dev libasound2-dev`, then run the two `sh build/...` steps directly.

`build.sh` prints the highest glibc symbol version the plugin uses (it must be ≤ 2.35 on webOS 24) and the plugin's md5. The link uses `--no-undefined` against the TV's libraries, so any symbol the TV lacks fails at build time, not on the TV.

## Install

Use the prebuilt `libgstlghdaudio.so` from [Releases](https://github.com/File48/webos-lghd/releases) (tested on LG C2 / webOS 24 only), or build it yourself as above.

```sh
sh install.sh root@TV_IP path/to/libgstlghdaudio.so
```

Then fully close and relaunch your player app.

### Check it works

Play something with TrueHD. The soundbar should report **multichannel PCM 5.1**. Then run:

```sh
ssh root@TV_IP 'grep -E "dvtag: first|send pcm|eARC" /tmp/lghd.log | tail -5'
ssh root@TV_IP 'sh /media/developer/lghd/lghd.sh status; tail -5 /tmp/lghd_boot.log'
```

You should see `send pcm/6 (initial)`, followed by `outputtype=2,1,6,0,0` (eARC set to multichannel PCM 5.1). For a Dolby Vision MKV you should also see `dvtag: first AU: rpu=1 ... -> DOLBY VISION`.

**TV setting:** *Sound → Advanced → Digital Sound Output* must be **Pass Through**. On one boot, `audiooutputd` came up in `pcm` mode even though the setting said Pass Through. The boot log warns if this happens; toggling the setting to Auto and back to Pass Through fixes it.

### Settings

Settings live in `/media/developer/lghd/lghd.conf` (`KEY=value`). They're read at the start of each playback. See [`tv/lghd.conf`](tv/lghd.conf) and the header of [`src/gstlghdaudiodec.c`](src/gstlghdaudiodec.c) for every option.

### Uninstall

```sh
sh uninstall.sh root@TV_IP            # deactivate now and at boot (files kept)
sh uninstall.sh root@TV_IP --purge    # also delete /media/developer/lghd
```

To skip it for a single boot: `ssh root@TV_IP 'touch /media/developer/lghd/disabled'`.

## Litefin

Litefin 2.0.20 needs two small client-side fixes to direct-play Dolby Vision MKVs on webOS 24:

1. **Rule removal.** Its "Dolby Vision MKV — Force Remux on WebOS < 25" rule makes Jellyfin reject direct play. You get HLS, and TrueHD is transcoded. Upstream master has already removed this rule.
2. **DV hint.** Its Dolby Vision hint is the bare `video/mp4; codecs="dvh1"`, and webOS 24's `canPlayType` rejects that. The patch builds the hint from the stream's profile and level, e.g. `dvh1.08.10`.

Litefin is MPL-2.0. This repo ships only the edits, which `litefin/make-patch.py` applies to your own copy of the app, checking md5s before and after:

```sh
A=/media/developer/apps/usr/palm/applications/org.litefin.app/js
scp root@TV_IP:$A/main-5d7218db.js root@TV_IP:$A/main-37bbcbe8.js .
python3 litefin/make-patch.py main-5d7218db.js main-37bbcbe8.js litefin/out
sh install.sh root@TV_IP        # copies litefin/out/* and mounts them
```

If Litefin updates, the md5 checks fail and the patches are skipped. Litefin then behaves as it does upstream.

## How it works / findings

See [FINDINGS.md](FINDINGS.md). It covers the webOS audio path, why `audiooutputd` picks stereo, how the plugin gets LG's pipeline ID and ADEC resource, the eARC ALSA values, pre-started pipelines, and why DV is lost in MKV.

## Licence

LGPL-2.1-or-later (see [LICENSE](LICENSE)). The plugin statically links an FFmpeg 7.1 build configured without `--enable-gpl` or `--enable-nonfree`. [`build/ffmpeg.sh`](build/ffmpeg.sh) reproduces it exactly from FFmpeg's source. `build/glibconfig-armel32.h` is a generated GLib header (LGPL).

This is not affiliated with LG, Dolby, DTS, Sonos, Jellyfin or Litefin.

## AI disclosure

This was investigated and written with an AI assistant (Claude), to solve one specific problem on one TV. That included reading LG's binaries to understand the audio path. Treat it accordingly: read the scripts before you run them as root, and expect your TV to differ.
