cmake_minimum_required(VERSION 3.20)
project(termvideo LANGUAGES C CXX)

# Encoder: modern C++23 (offline, unconstrained).
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Decoder + shared codec: portable C (must also build for 16-bit DOS toolchains).
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(src/common)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Measurement scaffolding: -DTVID_PROBE compiles in the encoder's experimental
# byte-accounting probes (e.g. the unshipped "drop per-frame length" lever) and
# the player's raw frame-dump mode (TVID_DUMP=1). Off by default; production
# binaries carry none of it. See CLAUDE.md > Measurement probes.
option(TVID_PROBE "Compile in measurement probes / frame-dump mode" OFF)

# Shared codec, split at the encode/decode line. src/common/ holds only the
# format headers + the DECODE half the player needs (portable C99, also the 16-bit
# DOS build set). Every COMPRESS half moved under src/encoder/ as C++ (the encoder
# is unconstrained). The wire-format constants/models shared by both halves live in
# private *_internal.h headers so the bytestream stays single-sourced.
set(TVID_COMMON_DEC
  src/common/codec.c
  src/common/lzss_dec.c
  src/common/range_dec.c
  src/common/entropy_dec.c
  src/common/huffman_dec.c
  src/common/adpcm_dec.c)
set(TVID_ENC_CODEC
  src/encoder/lzss_enc.cpp
  src/encoder/range_enc.cpp
  src/encoder/entropy_enc.cpp
  src/encoder/huffman_enc.cpp
  src/encoder/adpcm_enc.cpp)

# --- encoder (C++23) ---
add_executable(encoder
  src/encoder/encoder.cpp
  src/encoder/enc_stages.cpp
  src/encoder/quantize.cpp
  src/encoder/blockcoder.cpp
  ${TVID_COMMON_DEC}
  ${TVID_ENC_CODEC})

# --- player (portable C, ANSI backend on the host) ---
# Audio backend: AudioToolbox (AudioQueue) on Apple, a no-op stub elsewhere.
if(APPLE)
  set(TVID_AUDIO_BACKEND src/decoder/audio_coreaudio.c)
else()
  set(TVID_AUDIO_BACKEND src/decoder/audio_null.c)
endif()
add_executable(player
  src/decoder/player.c
  src/decoder/backend_ansi.c
  ${TVID_AUDIO_BACKEND}
  ${TVID_COMMON_DEC})
if(APPLE)
  target_link_libraries(player
    "-framework AudioToolbox" "-framework CoreFoundation")
endif()

# --- self-tests ---
# Roundtrip tests link the matching _dec (common C) + _enc (encoder C++) halves;
# the encode/decode split runs through these files, so a test needs both sides.
enable_testing()
# Block-frame round-trip exercises the C++ encoder against the C decoder, so
# it is a C++ target (links blockcoder + quantize + the portable codec).
add_executable(codec_test
  tests/codec_test.cpp
  src/encoder/blockcoder.cpp
  src/encoder/quantize.cpp
  src/common/codec.c)
target_include_directories(codec_test PRIVATE src/encoder)
add_test(NAME codec_roundtrip COMMAND codec_test)

add_executable(lzss_test tests/lzss_test.c src/common/lzss_dec.c src/encoder/lzss_enc.cpp)
add_test(NAME lzss_roundtrip COMMAND lzss_test)

add_executable(entropy_test
  tests/entropy_test.c
  src/common/entropy_dec.c src/encoder/entropy_enc.cpp
  src/common/huffman_dec.c src/encoder/huffman_enc.cpp
  src/common/lzss_dec.c src/encoder/lzss_enc.cpp)
add_test(NAME entropy_roundtrip COMMAND entropy_test)

add_executable(mode_rle_test tests/mode_rle_test.c)
add_test(NAME mode_rle_roundtrip COMMAND mode_rle_test)

add_executable(range_test tests/range_test.c src/common/range_dec.c src/encoder/range_enc.cpp)
add_test(NAME range_roundtrip COMMAND range_test)

add_executable(adpcm_test tests/adpcm_test.c src/common/adpcm_dec.c src/encoder/adpcm_enc.cpp)
target_link_libraries(adpcm_test m)
add_test(NAME adpcm_roundtrip COMMAND adpcm_test)

# Golden-byte test: pins the EXACT compressed output of each entropy method, so a
# refactor that still round-trips but changes the bytestream is caught.
add_executable(coder_golden_test
  tests/coder_golden_test.cpp
  src/common/lzss_dec.c src/encoder/lzss_enc.cpp
  src/common/range_dec.c src/encoder/range_enc.cpp
  src/common/entropy_dec.c src/encoder/entropy_enc.cpp
  src/common/huffman_dec.c src/encoder/huffman_enc.cpp)
add_test(NAME coder_golden COMMAND coder_golden_test)

# End-to-end seam: encoder planes -> entropy compress -> decompress -> block
# decode, asserting byte-identical frames across every method.
add_executable(encode_decode_test
  tests/encode_decode_test.cpp
  src/encoder/blockcoder.cpp src/encoder/quantize.cpp
  src/common/codec.c
  src/common/lzss_dec.c src/encoder/lzss_enc.cpp
  src/common/range_dec.c src/encoder/range_enc.cpp
  src/common/entropy_dec.c src/encoder/entropy_enc.cpp
  src/common/huffman_dec.c src/encoder/huffman_enc.cpp)
target_include_directories(encode_decode_test PRIVATE src/encoder)
add_test(NAME encode_decode COMMAND encode_decode_test)

if(TVID_PROBE)
  target_compile_definitions(encoder PRIVATE TVID_PROBE)
  target_compile_definitions(player PRIVATE TVID_PROBE)
endif()

# --- DOS player cross-build (opt-in: -DTVID_DOS=ON) ---
#
# Builds PLAYER.EXE, the 32-bit DOS/4GW player (text-mode video backend +
# Sound Blaster auto-init-DMA audio), using OpenWatcom's wcl386. With TVID_DOS
# on, CMake will fetch and bootstrap OpenWatcom v2 from source the first time
# (a long, one-off build), then cross-compile the DOS sources with it. This is
# entirely separate from the host targets above; leave it OFF for normal work.
# tools/build-dos.sh does the same thing against a wcl386 already on PATH and is
# the quicker path if you have OpenWatcom installed. See doc/dos-build.md.
option(TVID_DOS "Cross-build the DOS player (fetches+builds OpenWatcom)" OFF)
if(TVID_DOS)
  include(ExternalProject)

  # Where OpenWatcom's own bootstrap drops the built toolchain. On a 64-bit
  # Unix/macOS host that is rel/binl64 (Linux) or rel/armo64|binl64 depending on
  # arch; we probe a couple of candidates at build time and use whichever holds
  # wcl386. OWTOOLS=CLANG so it bootstraps with the system clang on macOS.
  set(OW_PREFIX  "${CMAKE_BINARY_DIR}/open-watcom")
  set(OW_SRC     "${OW_PREFIX}/src/openwatcom")
  set(OW_ROOT    "${OW_SRC}")               # OWROOT == the source tree
  set(OW_REL     "${OW_SRC}/rel")           # built release tree

  # The bootstrap: copy setvars.sh, point OWROOT at the tree, pick CLANG on
  # Apple, source it, then build.sh rel. Wrapped in a script so ExternalProject
  # can run it as one build step with the right environment.
  if(APPLE)
    set(OW_TOOLS "CLANG")
  else()
    set(OW_TOOLS "GCC")
  endif()
  # No parallel build. OpenWatcom's bootstrap makefile (bld/wmake/posmake) has
  # incomplete dependency ordering: generated headers like usage.gh (built from
  # usage.sp by wsplice) are not ordered before the .c files that #include them,
  # so a parallel `make -jN` races and dies with "usage.gh file not found". The
  # later `builder`/wmake walk has no -j knob either. The whole toolchain build is
  # therefore serial and slow — but one-off (cached under build-dos/open-watcom/).
  #
  # `build.sh rel` builds the compilers/tools *and* a browser-help doc step that
  # needs a configured DOSBOX (it dies with "Missing DOSBOX configuration"). We
  # don't need those docs — the cross-compiler (wcl386) is copied into rel/ before
  # that step runs — so we don't let the doc failure fail the whole bootstrap:
  # run build.sh, then succeed iff wcl386 actually landed in the release tree.
  file(WRITE "${CMAKE_BINARY_DIR}/ow-bootstrap.sh"
"#!/bin/sh
cd \"${OW_SRC}\"
export OWROOT=\"${OW_SRC}\"
export OWTOOLS=\"${OW_TOOLS}\"
. ./setvars.sh
export OWTOOLS=\"${OW_TOOLS}\"
./build.sh rel || echo 'build.sh exited non-zero (likely the optional DOSBOX doc step) — checking for wcl386'
for d in rel/binl64 rel/armo64 rel/binl; do
  if [ -x \"$d/wcl386\" ]; then
    echo \"openwatcom: wcl386 present at $d/wcl386\"
    exit 0
  fi
done
echo 'openwatcom: wcl386 was not produced — real build failure' >&2
exit 1
")

  ExternalProject_Add(openwatcom
    PREFIX            "${OW_PREFIX}"
    GIT_REPOSITORY    "https://github.com/open-watcom/open-watcom-v2"
    GIT_SHALLOW       TRUE
    SOURCE_DIR        "${OW_SRC}"
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE   TRUE
    BUILD_COMMAND     sh "${CMAKE_BINARY_DIR}/ow-bootstrap.sh"
    INSTALL_COMMAND   ""
    # By default ExternalProject re-runs its `update` step (a git fetch+checkout)
    # on EVERY build, hitting the network and re-touching the toolchain tree even
    # though it is already built. The toolchain is a one-off; pin the clone so the
    # download/update happen exactly once.
    UPDATE_DISCONNECTED TRUE
    UPDATE_COMMAND    "")
    # (No BUILD_BYPRODUCTS: wcl386's bin dir is host-arch-specific — binl64 on
    #  x86-64, armo64 on Apple silicon — so dos-cross.cmake resolves it at run
    #  time. The openwatcom target-level DEPENDS below sequences the build.)

  # Cross-compile the DOS player with the freshly built wcl386. The custom
  # command resolves the actual bin dir at run time (binl64 on x86-64 hosts,
  # armo64 on Apple silicon) and shells out to tools/build-dos.sh with WATCOM
  # pointed at the built release tree.
  set(DOS_EXE "${CMAKE_BINARY_DIR}/dos/PLAYER.EXE")
  add_custom_command(
    OUTPUT  "${DOS_EXE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/dos"
    COMMAND "${CMAKE_COMMAND}"
            -DWATCOM_REL=${OW_REL}
            -DSRC_ROOT=${CMAKE_SOURCE_DIR}
            -DOUT_DIR=${CMAKE_BINARY_DIR}/dos
            -P "${CMAKE_SOURCE_DIR}/tools/dos-cross.cmake"
    DEPENDS openwatcom
            "${CMAKE_SOURCE_DIR}/tools/dos-cross.cmake"
            "${CMAKE_SOURCE_DIR}/src/decoder/player.c"
            "${CMAKE_SOURCE_DIR}/src/decoder/backend_dos.c"
            "${CMAKE_SOURCE_DIR}/src/decoder/audio_dos.c"
    VERBATIM
    COMMENT "Cross-compiling DOS PLAYER.EXE with OpenWatcom")
  add_custom_target(dos_player ALL DEPENDS "${DOS_EXE}")
endif()
