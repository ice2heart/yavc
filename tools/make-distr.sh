#!/bin/sh
# make-distr.sh - assemble a self-contained DOS distribution directory.
#
# Collects the three things needed to play termvideo clips under DOSBox-X (or on
# real DOS) into ./distr:
#   - tools/dosbox-x.conf  -> distr/dosbox-x.conf
#   - the cross-built PLAYER.EXE
#   - every *.vi / *.tvid clip, renamed to DOS-legal 8.3 names
#
# DOS filenames are at most 8 chars + a 3-char extension, uppercase, and may not
# contain '.', '-', spaces, etc. in the base. So each clip's base name is
# uppercased, illegal characters dropped, truncated to 8 chars, and made unique
# on collision; .tvid becomes .VI (3-char extension) and .vi stays .VI.
#
# Usage:
#   tools/make-distr.sh                 # videos from ./videos -> ./distr
#   tools/make-distr.sh <srcdir> <out>  # override clip source and output dir
#
# Build PLAYER.EXE first (tools/build-dos.sh), then run this. A MANIFEST.TXT in
# the output records the original->8.3 name mapping.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:-"$ROOT/videos"}
OUT=${2:-"$ROOT/distr"}
CONF="$ROOT/tools/dosbox-x.conf"

# Locate PLAYER.EXE: prefer the build-dos.sh output, then the CMake DOS path.
PLAYER=""
for p in "$ROOT/build-dos/PLAYER.EXE" "$ROOT/build-dos/dos/PLAYER.EXE"; do
  [ -f "$p" ] && { PLAYER=$p; break; }
done
if [ -z "$PLAYER" ]; then
  echo "make-distr.sh: PLAYER.EXE not found. Build it first:" >&2
  echo "  tools/build-dos.sh" >&2
  exit 1
fi
[ -f "$CONF" ] || { echo "make-distr.sh: $CONF missing" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"
cp "$CONF" "$OUT/dosbox-x.conf"
cp "$PLAYER" "$OUT/PLAYER.EXE"

MANIFEST="$OUT/MANIFEST.TXT"
: > "$MANIFEST"

# Turn an arbitrary clip filename into a unique DOS 8.3 name in $OUT.
# Echoes the chosen 8.3 name.
dos_name() {
  base=$(basename "$1")
  stem=${base%.*}
  # Uppercase, then keep only A-Z 0-9 (drops '.', '-', '_', spaces, etc.).
  stem=$(printf '%s' "$stem" | tr '[:lower:]' '[:upper:]' | tr -cd 'A-Z0-9')
  [ -n "$stem" ] || stem=CLIP
  stem=$(printf '%s' "$stem" | cut -c1-8)
  cand="$stem.VI"
  # Disambiguate collisions: trim the stem and append a digit (keeps <=8 chars).
  n=1
  while [ -e "$OUT/$cand" ]; do
    suffix=$(printf '%d' "$n")
    keep=$(( 8 - ${#suffix} ))
    cand="$(printf '%s' "$stem" | cut -c1-"$keep")$suffix.VI"
    n=$((n + 1))
  done
  printf '%s' "$cand"
}

count=0
for f in "$SRC"/*.vi "$SRC"/*.tvid; do
  [ -e "$f" ] || continue          # no matches for a glob -> skip the literal
  name=$(dos_name "$f")
  cp "$f" "$OUT/$name"
  printf '%-28s -> %s\n' "$(basename "$f")" "$name" | tee -a "$MANIFEST"
  count=$((count + 1))
done

echo "----"
echo "distr: $OUT  ($count clip(s), PLAYER.EXE, dosbox-x.conf)"
echo "play:  cd $OUT && dosbox-x -conf dosbox-x.conf PLAYER.EXE <CLIP>.VI"
