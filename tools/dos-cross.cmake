# dos-cross.cmake - invoked via `cmake -P` from the TVID_DOS build to cross-
# compile the DOS player with a freshly built OpenWatcom tree.
#
# Inputs (set with -D on the cmake -P command line):
#   WATCOM_REL  the OpenWatcom release tree (its `rel` dir, holding binl64/...)
#   SRC_ROOT    the termvideo source root
#   OUT_DIR     where to write PLAYER.EXE
#
# We can't bake the wcl386 path at configure time because the toolchain doesn't
# exist until the openwatcom ExternalProject has built. This script runs as a
# build step, after that dependency, and resolves the host bin dir then.

# Find the host binary dir inside the release tree. 64-bit Linux/macOS-x86 ->
# binl64; Apple silicon -> armo64; fall back to binl (32-bit) just in case.
set(_bindir "")
foreach(cand binl64 armo64 binl)
  if(EXISTS "${WATCOM_REL}/${cand}/wcl386")
    set(_bindir "${WATCOM_REL}/${cand}")
    break()
  endif()
endforeach()
if(_bindir STREQUAL "")
  message(FATAL_ERROR
    "dos-cross: wcl386 not found under ${WATCOM_REL}/{binl64,armo64,binl}. "
    "Did the OpenWatcom bootstrap finish?")
endif()

# OpenWatcom needs WATCOM/PATH/INCLUDE in the environment to find its headers
# and the DOS/4GW stub. WATCOM points at the release tree root.
set(ENV{WATCOM}  "${WATCOM_REL}")
set(ENV{PATH}    "${_bindir}:$ENV{PATH}")
set(ENV{INCLUDE} "${WATCOM_REL}/h")
set(ENV{EDPATH}  "${WATCOM_REL}/eddat")

# Same source set and flags as tools/build-dos.sh.
set(SRC
  "${SRC_ROOT}/src/decoder/player.c"
  "${SRC_ROOT}/src/decoder/backend_dos.c"
  "${SRC_ROOT}/src/decoder/audio_dos.c"
  "${SRC_ROOT}/src/common/codec.c"
  "${SRC_ROOT}/src/common/lzss_dec.c"
  "${SRC_ROOT}/src/common/entropy_dec.c"
  "${SRC_ROOT}/src/common/huffman_dec.c"
  "${SRC_ROOT}/src/common/range_dec.c"
  "${SRC_ROOT}/src/common/range_ctx_dec.c"
  "${SRC_ROOT}/src/common/adpcm_dec.c")

execute_process(
  COMMAND "${_bindir}/wcl386"
          -bt=dos -l=dos4g -mf -5r -ox -ot -za99
          "-I${SRC_ROOT}/src/common" "-I${SRC_ROOT}/src/decoder"
          -fe=PLAYER.EXE
          ${SRC}
  WORKING_DIRECTORY "${OUT_DIR}"
  RESULT_VARIABLE   _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "dos-cross: wcl386 failed (exit ${_rc})")
endif()
message(STATUS "dos-cross: built ${OUT_DIR}/PLAYER.EXE")
