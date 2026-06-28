#!/usr/bin/env bash
# encode.sh - downscale a video with ffmpeg and encode it to .tvid.
#
# Usage: tools/encode.sh INPUT OUTPUT.tvid [--fps N] [--stable T] [--fit-size BYTES]
#                                          [--block-stable T] [--lambda L] [--dither]
#   --fps N         force a single frame rate (default 10)
#   --stable T      per-cell temporal hysteresis: a cell's target only changes when
#                   its averaged RGB moves > T (0..~96). Higher = smaller, less
#                   detail (default 32).
#   --lookahead N   peek N future frames: an excursion that reverts within the
#                   window is held as a transient blip (default 8, 0=off).
#   --shift R       SHIFT motion-vector search radius in cells (default 0=off).
#                   Codes a block as an offset copy of the previous frame; helps
#                   panning/translation, but taxes every SKIP leaf 1 bit so it can
#                   hurt low-motion content. Opt-in.
#   --block-stable T  block-level temporal hysteresis: distortion credit that lets
#                   a barely-changed quadtree block stay SKIP (kills block shimmer).
#   --lambda L      rate-distortion weight (default 24): higher spends more bytes
#                   chasing detail, lower favors SKIP/SOLID.
#   --quant Q       quantizer: nearest | joint | dither | bayer (default joint).
#                     joint  - brute-force best glyph+color per cell (default,
#                              usually smallest AND sharpest)
#                     nearest- independent glyph(luma)+color(hue) pick (fastest)
#                     dither - serpentine Floyd-Steinberg (smooth gradients)
#                     bayer  - 4x4 ordered dither (temporally stable, more SKIPs)
#   --dither        alias for --quant dither.
#   --mono          monochrome glyph-shape mode (v3): drop color, encode luma +
#                   sub-cell block/box glyphs for a smoother B&W image. Renders
#                   2x4 luma sub-pixels per cell; plays on a truecolor terminal.
#   --mono-boost    (mono only) convert the source to grayscale and auto-stretch
#                   its luma range with ffmpeg before encoding. Color clips have
#                   little luma contrast for the glyph match to grab, so they
#                   come out a blurry mess; this normalizes brightness per frame
#                   so edges/gradients survive. Implies --mono. No-op on sources
#                   already shot in high-contrast B&W.
#   --hi-res        use a 160x48 grid instead of the default 80x24 (smaller font).
#   --fit-size B    keep frame rate high: raise --lambda until the output fits B
#                   bytes, only dropping fps as a last resort.
#   --split         (DEFAULT) split block frames into structure + cell planes,
#                   entropy-coding each separately, plus auto-selected extras: a
#                   mode-tag plane, a zero-RLE of that mode plane, and a by-mode
#                   cell split (RAW vs SOLID/PAL2) (TVID_FLAG_SPLIT/_MODEPLANE/
#                   _MODERLE/_CELLSPLIT). ~25% smaller than plain whole-stream
#                   --compress on color video, the win growing with resolution.
#                   Each extra plane is auto-declined where it regresses (e.g.
#                   B&W). See doc/compression.md.
#   --no-split      fall back to plain whole-stream LZSS/Huffman compression.
#   --no-compress   store the block stream uncompressed (also disables --split).
#   --audio-rate N  audio sample rate in Hz (default 8000). The source's audio is
#                   extracted to mono s16 PCM at this rate, IMA-ADPCM encoded
#                   (~0.5 byte/sample), and embedded in the .tvid tail. ~8kHz mono
#                   fits a 3-min song in well under a megabyte. Also the primary
#                   *size* knob: audio is ~90% of file bytes and ADPCM size scales
#                   linearly with rate (6000 ~= 64%, 4000 ~= 43% vs 8000), at a
#                   quality cost. Lower it to fit an over-budget clip.
#   --audio-entropy range-code the ADPCM nibbles (audio codec 2). Lossless vs the
#                   default codec 1 (decoded PCM is bit-identical), ~10-12% off the
#                   whole file, DOS-decodable. Forwarded verbatim to the encoder.
#   --no-audio      do not embed audio (video only; the previous behavior). Also
#                   used automatically when the source has no audio track.
set -euo pipefail

FFMPEG="${FFMPEG:-ffmpeg}"
ENCODER="${ENCODER:-$(dirname "$0")/../build/encoder}"
# Default grid is 80x24; --hi-res bumps it to 160x48 (needs a smaller terminal
# font). Mono mode renders 2x4 luma sub-pixels per cell, so its source is larger.
COLS=80
ROWS=24

IN="${1:?input video required}"
OUT="${2:?output .tvid path required}"
shift 2

FPS=10
STABLE=16
LOOKAHEAD=8         # peek N future frames to hold transient cell churn (0=off)
SHIFT=0             # SHIFT motion-vector radius in cells (0=off; helps panning,
                    # hurts low-motion content because of a per-SKIP-leaf tax)
BLOCK_STABLE=2000
LAMBDA=6            # cost = LAMBDA*bits + distortion; LOWER = more detail
QUANT=mono          # v3 is always the sub-cell luma+glyph model (the old --quant
                    # color modes are retired); kept for the encoder's compat arg
MONO=1              # v3: luma + sub-cell shape glyphs (the only cell model)
MONO_BOOST=0        # --mono-boost: grayscale + per-frame luma normalize in ffmpeg
COLOR=0             # --color: add the xterm-256 color plane (TVID_FLAG_COLOR)
HIRES=0             # --hi-res: 160x48 grid instead of the default 80x24
EXTRA_FWD=""        # unrecognized flags, passed straight through to the encoder
AUDIO=1             # embed the source's audio (IMA-ADPCM); --no-audio disables
AUDIO_RATE=8000     # audio sample rate (Hz)
FIT=0
FIT_BUDGET=80000   # ~1.3MB; rest of the 1.44MB floppy is the decoder binary
COMPRESS=1           # whole-stream LZSS fallback (player decompresses transparently)
SPLIT=1              # DEFAULT: split structure/cell planes + auto-selected mode plane,
                     # each entropy-coded separately (TVID_FLAG_SPLIT/_MODEPLANE).
                     # ~20% smaller than plain --compress on color video, the win
                     # growing with resolution; the mode plane is auto-declined when
                     # it would regress (e.g. B&W). --no-split falls back to plain
                     # whole-stream compression. See doc/compression.md.
while [ $# -gt 0 ]; do
  case "$1" in
    --fps)          FPS="$2"; shift 2 ;;
    --stable)       STABLE="$2"; shift 2 ;;
    --lookahead)    LOOKAHEAD="$2"; shift 2 ;;
    --shift)        SHIFT="$2"; shift 2 ;;
    --block-stable) BLOCK_STABLE="$2"; shift 2 ;;
    --lambda)       LAMBDA="$2"; shift 2 ;;
    --dither)       QUANT=dither; shift ;;
    --quant)        QUANT="$2"; shift 2 ;;
    --mono)         MONO=1; QUANT=mono; shift ;;
    --mono-boost)   MONO=1; QUANT=mono; MONO_BOOST=1; shift ;;
    --color)        COLOR=1; shift ;;
    --hi-res)       HIRES=1; shift ;;
    --no-compress)  COMPRESS=0; SPLIT=0; shift ;;
    --split)        SPLIT=1; shift ;;
    --no-split)     SPLIT=0; shift ;;
    --audio-rate)   AUDIO_RATE="$2"; shift 2 ;;
    --no-audio)     AUDIO=0; shift ;;
    --fit-size)     FIT=1; FIT_BUDGET="$2"; shift 2 ;;
    # Anything else (e.g. --stats, or a knob this wrapper doesn't model) is
    # forwarded verbatim to the encoder. Lets one tune/measure without teaching
    # the wrapper every encoder flag. The encoder rejects truly-unknown flags.
    *) EXTRA_FWD="$EXTRA_FWD $1"; shift ;;
  esac
done

# --hi-res: 160x48 cells instead of the default 80x24.
if [ "$HIRES" -eq 1 ]; then COLS=160; ROWS=48; fi

# Extract the source's audio to raw mono s16le PCM once (reused across the
# fit-size sweep). If the source has no audio track, ffmpeg writes nothing and we
# fall back to video-only. The temp file is cleaned up on exit.
AUDIO_PCM=""
AUDIO_ARGS=""
if [ "$AUDIO" -eq 1 ]; then
  AUDIO_PCM="$(mktemp -t tvid-audio).s16"
  trap 'rm -f "$AUDIO_PCM"' EXIT
  "$FFMPEG" -hide_banner -loglevel error -i "$IN" -vn -ac 1 -ar "$AUDIO_RATE" \
    -f s16le "$AUDIO_PCM" 2>/dev/null || true
  if [ -s "$AUDIO_PCM" ]; then
    AUDIO_ARGS="--audio-pcm $AUDIO_PCM --audio-rate $AUDIO_RATE"
    echo "audio: $(wc -c < "$AUDIO_PCM") B PCM @ ${AUDIO_RATE} Hz mono -> ADPCM" >&2
  else
    echo "audio: source has no audio track; encoding video only" >&2
  fi
fi

encode_at() {
  local fps="$1" lambda="$2" out="$3"
  local extra=""
  # --split implies its own per-plane entropy, so it replaces --compress.
  if [ "$SPLIT" -eq 1 ]; then extra="$extra --split"
  elif [ "$COMPRESS" -eq 1 ]; then extra="$extra --compress"; fi
  if [ "$COLOR" -eq 1 ]; then extra="$extra --color"; fi
  # v3: each cell is a 2-wide x 4-tall sub-pixel block, so render (COLS*2)x(ROWS*4)
  # rgb24. The encoder derives luma (shape/brightness) from it and, with --color,
  # also box-averages each cell's hue. The 2:4 sub-pixel aspect matches the tall
  # text cell, so no extra vertical doubling/pad is needed here.
  local sw=$((COLS*2)) sh=$((ROWS*4))
  # --mono-boost: drop chroma and auto-stretch the luma histogram per frame so
  # low-contrast sources have edges/gradients for the glyph match to hit. Skipped
  # under --color (desaturating would throw away the hue we're trying to keep);
  # `normalize` rescales each frame's darkest/brightest to full black/white.
  local boost=""
  if [ "$MONO_BOOST" -eq 1 ] && [ "$COLOR" -eq 0 ]; then
    boost="hue=s=0,normalize=smoothing=10,eq=contrast=3.5,"
  fi
  "$FFMPEG" -hide_banner -loglevel error -i "$IN" -r "$fps" -vf \
    "${boost}scale=${sw}:${sh}:force_original_aspect_ratio=decrease,pad=${sw}:${sh}:(ow-iw)/2:(oh-ih)/2,format=rgb24" \
    -f rawvideo - | "$ENCODER" --fps "$fps" --stable "$STABLE" \
      --lookahead "$LOOKAHEAD" --shift "$SHIFT" --block-stable "$BLOCK_STABLE" \
      --lambda "$lambda" $extra $AUDIO_ARGS $EXTRA_FWD --cols "$COLS" --rows "$ROWS" --out "$out"
}

if [ "$FIT" -eq 0 ]; then
  encode_at "$FPS" "$LAMBDA" "$OUT"
  exit 0
fi

# Smoothness + detail first: hold fps and raise lambda (cheaper bits -> coarser
# blocks -> smaller file) before stepping fps down as a last resort. The embedded
# audio tail is part of $OUT, so the size check below already measures video +
# audio against the budget; the sweep tightens video until the combined file fits.
for fps in 12 8; do
  for lambda in 6 16 48; do
    echo "fit: trying ${fps} fps, lambda=${lambda} (budget ${FIT_BUDGET} B)..." >&2
    encode_at "$fps" "$lambda" "$OUT"
    sz=$(wc -c < "$OUT")
    if [ "$sz" -le "$FIT_BUDGET" ]; then
      echo "fit: ${fps} fps lambda=${lambda} fits (${sz} B <= ${FIT_BUDGET} B)" >&2
      exit 0
    fi
    echo "fit: ${fps} fps lambda=${lambda} too big (${sz} B)" >&2
  done
done
echo "fit: WARNING kept last result (${sz} B) — still over budget" >&2
