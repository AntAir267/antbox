#!/usr/bin/env bash
# The encoded fixtures of this test: half a second of a 1 kHz sine made here
# (no recording, nobody's music), encoded with ffmpeg. Run again only to
# change them; the files in data/ are what the test reads.
set -eu
cd "$(dirname "$0")/data"
python3 - <<'PY'
import math, struct, wave
for name, rate, ch in (("tone", 44100, 2), ("tone48", 48000, 2)):
    w = wave.open("/tmp/xst_%s.wav" % name, "wb")
    w.setnchannels(ch); w.setsampwidth(2); w.setframerate(rate)
    n = rate // 2
    w.writeframes(b"".join(struct.pack("<hh", *(2 * [int(16384 * math.sin(2 * math.pi * 1000 * i / rate))]))
                           for i in range(n)))
    w.close()
PY
ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }
ff -i /tmp/xst_tone.wav -c:a libmp3lame -b:a 64k -id3v2_version 3 \
   -metadata title="Árvíztűrő tükörfúrógép" -metadata artist="Test" tone_id3v23.mp3
ff -i /tmp/xst_tone48.wav -c:a libmp3lame -b:a 64k -id3v2_version 4 \
   -metadata title="Tone 48k" tone48_id3v24.mp3
ff -i /tmp/xst_tone.wav -c:a flac -metadata title="Flac Title" tone.flac
ff -i /tmp/xst_tone48.wav -c:a vorbis -strict -2 -b:a 64k -metadata title="Ogg Title" tone48.ogg
# macOS only (the system's decoder is what plays them there): Apple's encoder.
if command -v afconvert >/dev/null 2>&1; then
    afconvert -f m4af -d aac -b 96000 /tmp/xst_tone.wav tone_aac.m4a
    afconvert -f m4af -d alac /tmp/xst_tone.wav tone_alac.m4a
fi
rm -f /tmp/xst_tone.wav /tmp/xst_tone48.wav
ls -l
