#!/usr/bin/env bash
# encode-all.sh - batch-encode every video in a folder into four .vi variants.
#
# For each *.webm and *.mkv in the folder, produces four encodes via
# tools/encode.sh, written alongside the source with a variant suffix and the
# .vi extension:
#
#   sat.webm -> sat.mono.vi        (default 80x24, grayscale)
#               sat.mono-hires.vi  (160x48, grayscale)
#               sat.color.vi       (80x24, xterm-256 color plane)
#               sat.color-hires.vi (160x48, color)
#
# The encoder is single-threaded and a full roundtrip is slow, so the variants
# (and sources) are fanned out across cores -- a job per variant, capped at a
# concurrency limit, rather than run one at a time (see CLAUDE.md "Running many
# encodes"). Always re-encodes (overwrites existing .vi).
#
# Usage: tools/encode-all.sh [FOLDER] [-- <extra encode.sh flags>]
#   FOLDER   directory to scan (default: ./videos)
#   anything after `--` is forwarded to every encode.sh invocation, e.g.
#     tools/encode-all.sh videos -- --fps 15 --stable 16
#
# Env:
#   JOBS=N   max concurrent encodes (default: cores-2, min 1).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ENCODE="$HERE/encode.sh"

# --- args: optional FOLDER, then optional `-- <forwarded flags>` ---------------
FOLDER="videos"
if [ $# -gt 0 ] && [ "$1" != "--" ]; then FOLDER="$1"; shift; fi
if [ $# -gt 0 ] && [ "$1" = "--" ]; then shift; fi
FWD=("$@")   # forwarded verbatim to every encode.sh call

[ -d "$FOLDER" ] || { echo "encode-all: no such folder: $FOLDER" >&2; exit 1; }

# --- concurrency: encoder is single-threaded; leave 2 cores for ffmpeg/OS ------
if [ -z "${JOBS:-}" ]; then
  if command -v nproc >/dev/null 2>&1; then CORES=$(nproc)
  elif command -v sysctl >/dev/null 2>&1; then CORES=$(sysctl -n hw.ncpu)
  else CORES=4; fi
  JOBS=$((CORES - 2))
  [ "$JOBS" -lt 1 ] && JOBS=1
fi

# The four variants: name suffix -> the encode.sh flags that select it.
# --seg makes the variant a v4 segmented (streamable) stream so the DOS player's
# resident set is bounded per-segment instead of whole-movie. Applied to the
# color and hi-res variants, which are the large ones that OOM on DOS under v3;
# plain mono stays v3 (small enough to stay whole-resident).
VARIANT_NAMES=(mono       mono-hires        color           color-hires)
VARIANT_FLAGS=("--mono-boost"         "--hi-res --seg --mono-boost"  "--color --seg" "--color --hi-res --seg")

# --- collect sources -----------------------------------------------------------
shopt -s nullglob nocaseglob
SOURCES=("$FOLDER"/*.webm "$FOLDER"/*.mkv)
shopt -u nullglob nocaseglob
if [ ${#SOURCES[@]} -eq 0 ]; then
  echo "encode-all: no .webm/.mkv files in $FOLDER" >&2
  exit 1
fi

echo "encode-all: ${#SOURCES[@]} source(s) x 4 variants, up to $JOBS parallel jobs" >&2
[ ${#FWD[@]} -gt 0 ] && echo "encode-all: forwarding to encode.sh: ${FWD[*]}" >&2

# --- run one (source, variant) encode; logs go to a per-job .log --------------
run_one() {
  local in="$1" name="$2" flags="$3"; shift 3
  local base="${in%.*}"          # strip the source extension
  local out="${base}.${name}.vi"
  local log="${out}.log"
  echo "  [start] $out" >&2
  # `flags` is a space-separated list of literal encode.sh flags; word-split is
  # intended here. Forwarded args ("$@") keep their own quoting.
  # shellcheck disable=SC2086
  if "$ENCODE" "$in" "$out" $flags "$@" >"$log" 2>&1; then
    echo "  [done ] $out ($(wc -c <"$out" 2>/dev/null || echo '?') B)" >&2
  else
    echo "  [FAIL ] $out -- see $log" >&2
  fi
}

# --- fan out, capping in-flight jobs at $JOBS ----------------------------------
fail=0
for in in "${SOURCES[@]}"; do
  for i in "${!VARIANT_NAMES[@]}"; do
    # Throttle: wait for a slot when we're at the cap. `wait -n` isn't in bash 3.2
    # (the macOS system bash), so poll the running-job count with a short sleep.
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 0.3; done
    # ${FWD[@]+...} guard: an empty array is "unbound" under set -u in bash 3.2
    # (the macOS system bash), so expand to nothing when no flags were forwarded.
    run_one "$in" "${VARIANT_NAMES[$i]}" "${VARIANT_FLAGS[$i]}" ${FWD[@]+"${FWD[@]}"} &
  done
done
wait

echo "encode-all: all jobs finished" >&2
