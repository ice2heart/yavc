#!/usr/bin/env bash
# visualize.sh - downscale a video with ffmpeg (the SAME pipe encode.sh feeds the
# encoder) and pipe it into the codec visualizer instead, so encode-time and
# inspect-time parameters stay in sync.
#
# Usage: tools/visualize.sh INPUT [flags...]
#   Accepts the same codec/scaling knobs as tools/encode.sh:
#     --fps N            frame rate (default 10)
#     --stable T         per-cell temporal hysteresis (default 16)
#     --lookahead N      peek N future frames for transient churn (default 8)
#     --shift R          SHIFT motion-vector search radius in cells (default 0)
#     --block-stable T   block-level temporal hysteresis (default 2000)
#     --split-lookahead N quadtree-shape temporal coupling (default 2)
#     --lambda L         rate-distortion weight (default 6)
#     --color            add the xterm-256 color plane
#     --mono             monochrome glyph-shape mode (the v3 default)
#     --mono-boost       grayscale + per-frame luma normalize before encoding
#     --hi-res           use a 160x48 grid instead of the default 80x24
#   Visualizer-only:
#     --port P           HTTP port (default 8080, forwarded to the visualizer)
#     --webroot DIR      static asset root (forwarded to the visualizer)
#   Any other flag (e.g. --stats) is forwarded verbatim to the visualizer.
#
# Unlike encode.sh there is NO OUTPUT argument (the visualizer writes no file)
# and no audio handling (the visualizer ignores audio). It opens an HTTP server;
# open the printed URL to step through the encode stage-by-stage, frame-by-frame.
# See doc/visualizer.md.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FFMPEG="${FFMPEG:-ffmpeg}"
VISUALIZER="${VISUALIZER:-$ROOT/build-viz/visualizer}"
# Default grid is 80x24; --hi-res bumps it to 160x48 (needs a smaller terminal
# font). Mono mode renders 2x4 luma sub-pixels per cell, so its source is larger.
COLS=80
ROWS=24

IN="${1:?input video required}"
shift 1

FPS=10
STABLE=16
LOOKAHEAD=8         # peek N future frames to hold transient cell churn (0=off)
SHIFT=0             # SHIFT motion-vector radius in cells (0=off)
BLOCK_STABLE=2000
SPLIT_LOOKAHEAD=2   # quadtree-shape temporal coupling (matches the encoder default)
LAMBDA=6            # cost = LAMBDA*bits + distortion; LOWER = more detail
MONO_BOOST=0        # --mono-boost: grayscale + per-frame luma normalize in ffmpeg
COLOR=0             # --color: add the xterm-256 color plane
HIRES=0             # --hi-res: 160x48 grid instead of the default 80x24
EXTRA_FWD=""        # unrecognized flags + visualizer-only flags, passed through

while [ $# -gt 0 ]; do
  case "$1" in
    --fps)             FPS="$2"; shift 2 ;;
    --stable)          STABLE="$2"; shift 2 ;;
    --lookahead)       LOOKAHEAD="$2"; shift 2 ;;
    --shift)           SHIFT="$2"; shift 2 ;;
    --block-stable)    BLOCK_STABLE="$2"; shift 2 ;;
    --split-lookahead) SPLIT_LOOKAHEAD="$2"; shift 2 ;;
    --lambda)          LAMBDA="$2"; shift 2 ;;
    --color)           COLOR=1; shift ;;
    --mono)            shift ;;   # v3 is always mono-shape; accepted for parity
    --mono-boost)      MONO_BOOST=1; shift ;;
    --hi-res)          HIRES=1; shift ;;
    # Visualizer-only knobs and any other flag (e.g. --stats, --port, --webroot)
    # are forwarded verbatim. --port/--webroot take a value, so grab the next arg.
    --port|--webroot)  EXTRA_FWD="$EXTRA_FWD $1 $2"; shift 2 ;;
    *) EXTRA_FWD="$EXTRA_FWD $1"; shift ;;
  esac
done

# --hi-res: 160x48 cells instead of the default 80x24.
if [ "$HIRES" -eq 1 ]; then COLS=160; ROWS=48; fi

# Auto-build the visualizer if it is not present. The first configure fetches
# civetweb via CMake FetchContent, so it needs network the first time.
if [ ! -x "$VISUALIZER" ]; then
  echo "visualize: $VISUALIZER not found; building it (first configure needs network for civetweb)..." >&2
  cmake -S "$ROOT" -B "$ROOT/build-viz" -DTVID_VIZ=ON
  cmake --build "$ROOT/build-viz" --target visualizer
fi

# v3: each cell is a 2-wide x 4-tall sub-pixel block, so render (COLS*2)x(ROWS*4)
# rgb24 -- exactly the pipe encode.sh feeds the encoder.
SW=$((COLS*2)) SH=$((ROWS*4))

extra=""
if [ "$COLOR" -eq 1 ]; then extra="$extra --color"; fi
# --mono-boost: drop chroma and auto-stretch the luma histogram per frame.
# Skipped under --color (desaturating would throw away the hue we want to keep).
boost=""
if [ "$MONO_BOOST" -eq 1 ] && [ "$COLOR" -eq 0 ]; then
  boost="hue=s=0,normalize=smoothing=10,eq=contrast=3.5,"
fi

# Run from the repo root: the visualizer's default --webroot is relative.
cd "$ROOT"

"$FFMPEG" -hide_banner -loglevel error -i "$IN" -r "$FPS" -vf \
  "${boost}scale=${SW}:${SH}:force_original_aspect_ratio=decrease,pad=${SW}:${SH}:(ow-iw)/2:(oh-ih)/2,format=rgb24" \
  -f rawvideo - | "$VISUALIZER" --fps "$FPS" --stable "$STABLE" \
    --lookahead "$LOOKAHEAD" --shift "$SHIFT" --block-stable "$BLOCK_STABLE" \
    --split-lookahead "$SPLIT_LOOKAHEAD" --lambda "$LAMBDA" \
    $extra $EXTRA_FWD --cols "$COLS" --rows "$ROWS"
