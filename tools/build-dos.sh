#!/bin/sh
# build-dos.sh - cross-build the termvideo player for DOS (32-bit, DOS/4GW).
#
# Produces PLAYER.EXE: a DOS/4GW-extended 386 protected-mode binary with the
# text-mode video backend (backend_dos.c) and the Sound Blaster audio backend
# (audio_dos.c). Run it under DOSBox-X or on real hardware with a DOS/4GW-class
# DPMI host (DOS/4GW ships bundled into the .exe by the linker).
#
# Toolchain: OpenWatcom v2 (wcl386). Set WATCOM to its install root, or have
# `wcl386` on PATH with the env already sourced (owsetenv.sh). On macOS install
# with: brew install --cask open-watcom  (or use the official installer).
#
# Usage:
#   tools/build-dos.sh                # -> build-dos/PLAYER.EXE
#   WATCOM=/path/to/watcom tools/build-dos.sh
#
# This is a host cross-compile; nothing here touches the CMake host build.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build-dos"
mkdir -p "$OUT"

# Locate the OpenWatcom toolchain. Honour an explicit WATCOM root; otherwise
# trust a wcl386 already on PATH.
if [ -n "$WATCOM" ]; then
  export WATCOM
  # Bin dir is host-arch-specific: armo64 (Apple silicon), binl64 (x86-64 Unix),
  # binl (32-bit). Put them all on PATH; wcl386 lives in whichever matches.
  export PATH="$WATCOM/armo64:$WATCOM/binl64:$WATCOM/binl:$WATCOM/bino64:$WATCOM/binnt64:$PATH"
  export INCLUDE="$WATCOM/h"
  export EDPATH="$WATCOM/eddat"
fi

if ! command -v wcl386 >/dev/null 2>&1; then
  echo "build-dos.sh: wcl386 not found. Install OpenWatcom and set WATCOM, or" >&2
  echo "  source its owsetenv.sh so wcl386 is on PATH." >&2
  exit 1
fi

# Source set: the portable player + DOS backends + shared codec DECODE half.
# Only the *_dec.c files build here -- the compress halves are C++ (encoder-only)
# and never ship on the floppy. mode_rle is header-only (static inline); no .c for
# it. The host audio_coreaudio/null and the ANSI backend are deliberately excluded.
SRC="
  $ROOT/src/decoder/player.c
  $ROOT/src/decoder/backend_dos.c
  $ROOT/src/decoder/audio_dos.c
  $ROOT/src/common/codec.c
  $ROOT/src/common/lzss_dec.c
  $ROOT/src/common/entropy_dec.c
  $ROOT/src/common/huffman_dec.c
  $ROOT/src/common/range_dec.c
  $ROOT/src/common/range_ctx_dec.c
  $ROOT/src/common/adpcm_dec.c
"

# Flags:
#   -bt=dos -l=dos4g  : DOS target, DOS/4GW extender stub linked in.
#   -mf               : flat 32-bit memory model.
#   -5r -fp5          : 586 register calls, 587 FP (FP only used at build time).
#   -ox -ot           : full optimisation, favour speed.
#   -za99             : C99 (player + codec assume C99).
#   -DTVID_* off      : production build (no probes).
cd "$OUT"
wcl386 -bt=dos -l=dos4g -mf -5r -ox -ot -za99 \
  -I"$ROOT/src/common" -I"$ROOT/src/decoder" \
  -fe=PLAYER.EXE \
  $SRC

echo "built $OUT/PLAYER.EXE"
