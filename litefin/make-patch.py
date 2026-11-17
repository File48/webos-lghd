#!/usr/bin/env python3
"""Make the two patched Litefin 2.0.20 bundle files from YOUR installed copy.

Litefin is MPL-2.0; this repo ships only the edits, never Litefin's files.

  # copy the originals off the TV
  scp root@TV:/media/developer/apps/usr/palm/applications/org.litefin.app/js/main-5d7218db.js .
  scp root@TV:/media/developer/apps/usr/palm/applications/org.litefin.app/js/main-37bbcbe8.js .
  python3 litefin/make-patch.py main-5d7218db.js main-37bbcbe8.js out/

Patch 1 (main-5d7218db.js): disable the "Dolby Vision MKV - Force Remux on WebOS < 25"
  CodecProfile, which makes Jellyfin refuse direct play of DV MKVs
  (VideoRangeTypeNotSupported -> HLS -> TrueHD transcoded). Already removed upstream.
Patch 2 (main-37bbcbe8.js): build the DV canPlayType hint from the stream's
  DvProfile/DvLevel (dvh1.08.10 ...) instead of bare "dvh1", which webOS 24 rejects.
  Logs each candidate as "lghd-patch: canPlayType(...)".
"""
import hashlib, os, sys

def md5(b): return hashlib.md5(b).hexdigest()

PATCHES = {
    "main-5d7218db.js": dict(
        orig="5e97eccfcf0ba768d21fcd20c1e8e7c4", out="352d968667fe96551c6635ce11948c6a",
        old='V&&d.webosVersion<25&&Y.push({Type:"Video",Container:"mkv"',
        new='V&&d.webosVersion<0&&Y.push({Type:"Video",Container:"mkv"'),
    "main-37bbcbe8.js": dict(
        orig="24b7ac5527c0f03f65ce36ede7f4f63b", out="b75b606d3fbb0e2f7948680b7227b8f8",
        old="const t='video/mp4; codecs=\"dvh1\"'",
        new=("const t=(()=>{const V=document.createElement(\"video\"),P=s?.DvProfile,L=s?.DvLevel,"
             "PP=null!=P?String(P).padStart(2,\"0\"):\"08\",LL=null!=L?String(L).padStart(2,\"0\"):\"06\","
             "C=[`video/mp4; codecs=\"dvh1.${PP}.${LL}\"`,`video/mp4; codecs=\"dvhe.${PP}.${LL}\"`,"
             "`video/mp4; codecs=\"dvh1.${PP}.06\"`,`video/mp4; codecs=\"dvhe.${PP}.06\"`,"
             "'video/mp4; codecs=\"dvh1\"'];for(const X of C){let R=\"\";try{R=V.canPlayType(X)}catch(e){}"
             "d.info(`lghd-patch: canPlayType(${X}) = \"${R}\"`);if(\"\"!==R)return X}return C[C.length-1]})();"
             "d.info(`lghd-patch: DV hint chosen: ${t}`)")),
}

def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    outdir = sys.argv[3]
    os.makedirs(outdir, exist_ok=True)
    for path in sys.argv[1:3]:
        name = os.path.basename(path)
        p = PATCHES.get(name) or sys.exit(f"{name}: not a file this patch knows")
        src = open(path, "rb").read()
        if md5(src) != p["orig"]:
            sys.exit(f"{name}: md5 {md5(src)} is not Litefin 2.0.20 ({p['orig']}) - not patching")
        old, new = p["old"].encode(), p["new"].encode()
        if src.count(old) != 1:
            sys.exit(f"{name}: expected exactly one match, found {src.count(old)}")
        out = src.replace(old, new)
        if md5(out) != p["out"]:
            sys.exit(f"{name}: result md5 {md5(out)} != expected {p['out']}")
        open(os.path.join(outdir, name), "wb").write(out)
        print(f"OK {name} -> {os.path.join(outdir, name)} ({p['out']})")

main()
