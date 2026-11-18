# Findings (LG C2, webOS 24 / SDK 9.2.2)

These notes come from one TV. Where the TV's own binaries showed something, it's stated as fact. Anything inferred is marked as such.

## Audio path for app playback

```
app (Chromium/WAM) → uMediaServer → starfish-media-pipeline (one process per playback)
  playbin/decodebin3 → LG matroskademux → decoder → audiosink (GstLgAudioRender) → ADEC0/ADEC1
                                      └ com.webos.avconnector._xxx (inside the pipeline process)
                                          → luna /tv/setQualityAttribute audioInputInfo → audiod
                                          → audiooutputd /quality/setQualityAttribute (adds activeState:true)
```

- The pipeline process gets its ID in argv: `-scom.webos.pipeline._XXXX`.
- The ADEC resource can be read from the sink's `index` property. Reading `resource-info` back returns NULL.
- The stock registry has no element that handles `audio/x-true-hd`, so apps can't play TrueHD and Jellyfin transcodes it.
- LG's `matroskademux` outputs DTS (core and DTS-HD MA) as `audio/x-unknown, codec-id=A_DTS`. LG's own `dts_audiodec` (rank secondary) only accepts `audio/x-dts`/`x-dtsh`/`x-dtsl`/..., so nothing links and playback is silent. The plugin accepts the `x-unknown, codec-id=A_DTS` caps directly.
- Litefin 2.0.20 assumes Chromium drops DTS/TrueHD/FLAC from `video.audioTracks`, so for a non-default track of those codecs it asks Jellyfin to remux, which transcodes them. On webOS 24 the native list includes them (e.g. 3 tracks for TrueHD + 2×AC3), and switching natively works, at start and mid-playback; the decoder renegotiates and eARC stays on multichannel PCM. Litefin patch 3 skips the remux on the webOS backend.

## Why the TV picks stereo for decoded audio

avconnector reports the **source** codec to audiooutputd, e.g. `"true-hd"` with 8 channels, or `"(null)"` for codecs it can't name. Inside audiooutputd, libumi only selects multichannel PCM when **all** of these hold:

- the input codec is exactly `"pcm"`;
- the digital sound output mode isn't `pcm` (Auto and Pass Through both work; Auto is recommended because Pass Through sends Dolby Digital Plus Atmos from apps as stereo PCM);
- the channel count is 2, 6 or 8, and the sink device supports it (from `setExternalArcSettings` / the eARC capability);
- `activeState` is true. This is a global flag that defaults to true.

The fix is to send a second `setAudioInputInfo` yourself, using LG's own pipeline ID and resource, with `codec:"pcm", channel:6`. The pipeline ID comes from argv and the resource from the sink's `index`. Send it about 1 s after the first decoded output. This is what the plugin does:

```json
{"categoryType":"audioInputInfo","requestType":"setAudioInputInfo","activeState":true,
 "pipelineInfo":{"pipelineId":"_XXXX","streamType":"umimedia","contentType":"movie","sourceType":"MEDIA",
   "resource":"ADEC0","sourceAttribute":"MEDIA","reset":false,"gameInfo":{"isGameMode":false}},
 "categoryInfo":{"codec":"pcm","channel":6,"immersive":"none"}}
```
It goes to `luna://com.webos.service.audiooutput/quality/setQualityAttribute`.

Two more details:

- A call with `reset:true` never reaches the category update, so `activeState` is ignored on reset calls.
- A reset on an empty map returns errorCode 20 ("Driver error").

## eARC ALSA controls (card 0)

`numid=91` is `Sndout EARC Outputtype`; `numid=90` is `Sndout EARC OnOff`. Values seen for `numid=91`:

| value | Sonos reports |
|---|---|
| `2,1,6,0,0` | Multichannel PCM 5.1 |
| `2,1,0,0,1` | Stereo PCM (default) |
| `2,2,0,0,0` | Dolby Digital 5.1 (AC3 bitstream) |
| `2,7,0,0,1` | "Dolby Multichannel PCM" (MAT) |

Writing these controls directly silences the link. Switching through audiooutputd works mid-playback. The plugin only ever **reads** them.

## PCM layout

LG's DSP swaps centre and LFE compared with WAV order. The plugin outputs 8 slots: `FL FR LFE FC RL RR 0 0`, as S32LE, with `pcm-multi-ch=TRUE` set on the sink.

## Plugin loading traps

- **Pre-started pipelines.** LG starts `starfish-media-pipeline` processes ahead of time, and they read the registry once at startup (`GST_REGISTRY_UPDATE=no`). After you install, idle pre-started processes have to be stopped. An idle one has only the threads `starfish-media-`, `tLibSystrim` and `tFragmentation`.
- **Registry location.** The registry is `/mnt/flash/data/gst_1_0_registry.arm.bin`, which resolves to `/mnt/lg/flash/data/...`, and that resolved path is what `/proc/mounts` shows. Bind-mounting a RAM copy works and is undone by a reboot.
- **Logging.** PmLogDaemon only passes messages on its whitelist (`/etc/PmLogDaemon/whitelist.txt`), so libumi INFO logs never appear in the system log. The plugin logs to its own file.
- **Startup hook.** Homebrew Channel's `/var/lib/webosbrew/init.d` runs scripts with run-parts, so filenames must not contain dots.
- **Digital Sound Output resets at boot.** `soundOutputDigital` is stored per input: the settings service takes a `dimension` of `{"audioInput":"media"}` for apps and `hdmi1`–`hdmi4` for the HDMI inputs, not `hdmi`. Its description says `"volatile": false`, but every boot brings it back as `passThrough` on all inputs, and no service logs writing it. A runtime `setSystemSettings` survives input switching, so the boot hook sets `auto` on all five inputs.

## Dolby Vision in MKV

- LG's `matroskademux` outputs `video/x-h265, format=HEVC, container=mkv` with **no** Dolby Vision fields. LG's `qtdemux` outputs `format=dvh1` and `colorimetry=bt2100-pq` for a dvh1 MP4.
- `libgstlxvideodec.so` provides `dvbin` (the "Dolby-Vision splitter, metaparser, decoder", rank 291) and `lxvideodec`.
  - `dvbin` only takes the DV path if the caps are h264/h265 **and** contain `dolby-vision=(boolean)TRUE`.
  - `lxvideodec` reads `dolby-vision`, `dolby-vision-profile` and `format`.
- **Fix:** `lghddvtag` holds back the caps until the first access unit arrives, scans it for NAL type 62 (RPU) and 63 (EL), then sets `dolby-vision=TRUE`, `dolby-vision-profile=8` (7 if an EL is present; profile 7 untested) and `format=dvh1`. With that, the TV shows Dolby Vision on direct-played MKV.
- **Browser side:** `canPlayType('video/mp4; codecs="dvh1"')` returns `""`. `canPlayType('video/mp4; codecs="dvh1.08.10"')` returns `"probably"`.

## Other

- With TrueHD in MP4, `qtdemux` outputs `audio/x-gst-fourcc-mlpa`, which this plugin doesn't handle. MKV is the tested container.
- Jellyfin only logs StreamBuilder decisions (`TranscodeReasons`) at Debug level.
